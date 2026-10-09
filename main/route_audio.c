#include "route_audio.h"

#include "bsp_audio.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define ROUTE_AUDIO_QUEUE_DEPTH 2
#define ROUTE_AUDIO_PCM_SAMPLES 512

typedef struct {
    const uint8_t *data;
    uint32_t length;
    uint16_t sample_rate;
    uint32_t generation;
} route_audio_job_t;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_idle;
static TaskHandle_t s_task;
static volatile uint32_t s_generation;
static volatile bool s_playing;
static int16_t s_pcm[ROUTE_AUDIO_PCM_SAMPLES];

static const int s_index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8,
    -1, -1, -1, -1, 2, 4, 6, 8,
};

static const int s_step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
    34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
    157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
    598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878,
    2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894,
    6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818,
    18500, 20350, 22385, 24623, 27086, 29794, 32767,
};

static int16_t decode_nibble(uint8_t nibble, int *predictor, int *index)
{
    int step = s_step_table[*index];
    int diff = step >> 3;
    if (nibble & 1U) diff += step >> 2;
    if (nibble & 2U) diff += step >> 1;
    if (nibble & 4U) diff += step;
    if (nibble & 8U) *predictor -= diff;
    else *predictor += diff;

    if (*predictor > 32767) *predictor = 32767;
    if (*predictor < -32768) *predictor = -32768;

    *index += s_index_table[nibble & 0x0FU];
    if (*index < 0) *index = 0;
    if (*index > 88) *index = 88;
    return (int16_t)*predictor;
}

static bool cancelled(uint32_t generation)
{
    return generation != s_generation;
}

static void play_ima(const route_audio_job_t *job)
{
    if (job->length < 4U || job->sample_rate < 4000U || job->sample_rate > 48000U) return;
    int predictor = (int16_t)((uint16_t)job->data[0] | ((uint16_t)job->data[1] << 8));
    int index = job->data[2];
    if (index < 0 || index > 88) return;

    if (bsp_audio_set_format(job->sample_rate, 16, 1) != ESP_OK) return;
    bsp_audio_set_volume(78);

    size_t out_count = 0;
    s_pcm[out_count++] = (int16_t)predictor;
    const uint8_t *encoded = job->data + 4;
    const uint8_t *end = job->data + job->length;

    while (encoded < end && !cancelled(job->generation)) {
        const uint8_t byte = *encoded++;
        s_pcm[out_count++] = decode_nibble(byte & 0x0FU, &predictor, &index);
        if (out_count == ROUTE_AUDIO_PCM_SAMPLES) {
            if (bsp_audio_write(s_pcm, out_count * sizeof(s_pcm[0])) != ESP_OK) return;
            out_count = 0;
            if (cancelled(job->generation)) return;
        }
        s_pcm[out_count++] = decode_nibble((byte >> 4) & 0x0FU, &predictor, &index);
        if (out_count == ROUTE_AUDIO_PCM_SAMPLES) {
            if (bsp_audio_write(s_pcm, out_count * sizeof(s_pcm[0])) != ESP_OK) return;
            out_count = 0;
        }
    }

    if (out_count > 0U && !cancelled(job->generation)) {
        (void)bsp_audio_write(s_pcm, out_count * sizeof(s_pcm[0]));
    }
}

static void audio_task(void *arg)
{
    (void)arg;
    route_audio_job_t job;
    for (;;) {
        if (xQueueReceive(s_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        s_playing = true;
        if (!cancelled(job.generation)) play_ima(&job);
        s_playing = false;
        xSemaphoreGive(s_idle);
    }
}

esp_err_t route_audio_init(void)
{
    if (s_task != NULL) return ESP_OK;
    s_queue = xQueueCreate(ROUTE_AUDIO_QUEUE_DEPTH, sizeof(route_audio_job_t));
    s_idle = xSemaphoreCreateBinary();
    if (s_queue == NULL || s_idle == NULL) {
        if (s_queue != NULL) vQueueDelete(s_queue);
        if (s_idle != NULL) vSemaphoreDelete(s_idle);
        s_queue = NULL;
        s_idle = NULL;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(audio_task, "route_audio", 4096, NULL, 5, &s_task) != pdPASS) {
        vQueueDelete(s_queue);
        vSemaphoreDelete(s_idle);
        s_queue = NULL;
        s_idle = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t route_audio_play_cue(const route_view_t *route, const route_cue_t *cue)
{
    if (s_task == NULL || route == NULL || cue == NULL) return ESP_ERR_INVALID_STATE;
    if (cue->audio_codec != ROUTE_AUDIO_IMA_ADPCM4 || cue->audio_length < 4U) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    const uint8_t *audio = route_view_slice(route, cue->audio_offset, cue->audio_length);
    if (audio == NULL) return ESP_ERR_INVALID_ARG;

    ++s_generation;
    xQueueReset(s_queue);
    (void)xSemaphoreTake(s_idle, 0);
    route_audio_job_t job = {
        .data = audio,
        .length = cue->audio_length,
        .sample_rate = cue->sample_rate_hz,
        .generation = s_generation,
    };
    if (xQueueSend(s_queue, &job, 0) != pdTRUE) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

esp_err_t route_audio_stop(uint32_t timeout_ms)
{
    ++s_generation;
    if (s_queue != NULL) xQueueReset(s_queue);
    if (!s_playing) return ESP_OK;
    if (s_idle == NULL ||
        xSemaphoreTake(s_idle, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

bool route_audio_is_playing(void)
{
    return s_playing;
}
