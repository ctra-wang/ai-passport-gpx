#include "route_app.h"

#include "bsp_audio.h"
#include "bsp_battery.h"
#include "bsp_button.h"
#include "bsp_display.h"
#include "route_audio.h"
#include "route_ble.h"
#include "route_nav.h"
#include "route_protocol.h"
#include "route_storage.h"
#include "route_ui.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define APP_QUEUE_DEPTH 12
#define APP_RENDER_PERIOD_MS 250U
#define APP_GPS_TIMEOUT_MS 5000U
#define APP_BATTERY_PERIOD_MS 10000U

typedef enum {
    APP_EVENT_BUTTON = 1,
    APP_EVENT_BLE_FRAME,
    APP_EVENT_LINK,
} app_event_type_t;

typedef struct {
    app_event_type_t type;
    union {
        struct {
            bsp_btn_t button;
            bsp_btn_ev_t action;
        } button;
        struct {
            uint16_t size;
            uint8_t data[ROUTE_BLE_MAX_RX];
        } ble;
        struct {
            bool connected;
        } link;
    } data;
} app_event_t;

static const char *TAG = "route_app";
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static volatile bool s_ready;
static bool s_ble_connected;
static bool s_gps_valid;
static bool s_audio_ready;
static bool s_battery_ready;
static route_location_t s_location;
static uint32_t s_last_location_ms;
static route_nav_t s_nav;
static bool s_nav_ready;
static int32_t s_last_cue = -1;
static int s_battery_percent = -1;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static uint16_t clamp_dm(float meters)
{
    if (meters <= 0.0f) return 0U;
    const float dm = meters * 10.0f;
    return dm >= 65535.0f ? 65535U : (uint16_t)(dm + 0.5f);
}

static void send_status(uint8_t reply_to, route_status_code_t code)
{
    uint8_t packet[18] = {0};
    packet[0] = ROUTE_MSG_STATUS;
    packet[1] = reply_to;
    packet[2] = (uint8_t)code;

    uint8_t flags = 0;
    const route_view_t *route = route_storage_view();
    if (route != NULL) flags |= ROUTE_STATUS_FLAG_HAS_ROUTE;
    if (route_storage_upload_active()) flags |= ROUTE_STATUS_FLAG_UPLOADING;
    if (s_gps_valid) flags |= ROUTE_STATUS_FLAG_GPS_VALID;
    if (s_nav_ready && s_nav.offroute) flags |= ROUTE_STATUS_FLAG_OFF_ROUTE;
    if (route_audio_is_playing()) flags |= ROUTE_STATUS_FLAG_AUDIO;
    packet[3] = flags;

    route_wire_put_u32(packet + 4, route_storage_upload_next_offset());
    route_wire_put_u32(packet + 8, route != NULL ? route->header.route_id : 0U);
    route_wire_put_u32(packet + 12, s_nav_ready ? s_nav.route_m : 0U);
    route_wire_put_u16(packet + 16, s_nav_ready ? clamp_dm(s_nav.cross_track_m) : 0U);
    (void)route_ble_send_status(packet, sizeof(packet));
}

static void on_button(bsp_btn_t button, bsp_btn_ev_t action, void *context)
{
    (void)context;
    if (!s_ready || s_queue == NULL) return;
    const app_event_t event = {
        .type = APP_EVENT_BUTTON,
        .data.button = {.button = button, .action = action},
    };
    (void)xQueueSend(s_queue, &event, 0);
}

static void on_ble_rx(const uint8_t *data, size_t size, void *context)
{
    (void)context;
    if (!s_ready || s_queue == NULL || data == NULL ||
        size == 0U || size > ROUTE_BLE_MAX_RX) return;
    app_event_t event = {.type = APP_EVENT_BLE_FRAME};
    event.data.ble.size = (uint16_t)size;
    memcpy(event.data.ble.data, data, size);
    (void)xQueueSend(s_queue, &event, 0);
}

static void on_ble_link(bool connected, void *context)
{
    (void)context;
    if (!s_ready || s_queue == NULL) return;
    const app_event_t event = {
        .type = APP_EVENT_LINK,
        .data.link = {.connected = connected},
    };
    (void)xQueueSend(s_queue, &event, 0);
}

static void reset_navigation(void)
{
    const route_view_t *route = route_storage_view();
    s_nav_ready = route != NULL;
    s_gps_valid = false;
    s_last_location_ms = 0U;
    s_last_cue = -1;
    if (route != NULL) route_nav_init(&s_nav, route);
    else memset(&s_nav, 0, sizeof(s_nav));
    route_ui_recenter(0U);
}

static void replay_last_cue(void)
{
    const route_view_t *route = route_storage_view();
    if (!s_audio_ready || route == NULL || s_last_cue < 0) return;
    route_cue_t cue;
    if (route_view_get_cue(route, (uint32_t)s_last_cue, &cue)) {
        (void)route_audio_play_cue(route, &cue);
    }
}

static void play_special_cue(uint8_t type)
{
    const route_view_t *route = route_storage_view();
    if (!s_audio_ready || route == NULL) return;

    for (uint32_t i = 0; i < route->header.cue_count; ++i) {
        route_cue_t cue;
        if (!route_view_get_cue(route, i, &cue)) return;
        if (cue.type == type && cue.audio_length > 0U) {
            (void)route_audio_play_cue(route, &cue);
            return;
        }
    }
}

static void process_button(const app_event_t *event)
{
    const bsp_btn_t button = event->data.button.button;
    const bsp_btn_ev_t action = event->data.button.action;
    if (action == BSP_BTN_CLICK && button == BSP_BTN_UP) route_ui_pan(+1);
    else if (action == BSP_BTN_CLICK && button == BSP_BTN_DOWN) route_ui_pan(-1);
    else if (action == BSP_BTN_LONG && button == BSP_BTN_UP) route_ui_zoom(+1);
    else if (action == BSP_BTN_LONG && button == BSP_BTN_DOWN) route_ui_zoom(-1);
    else if (action == BSP_BTN_CLICK && button == BSP_BTN_OK) {
        route_ui_recenter(s_nav_ready ? s_nav.route_m : 0U);
    } else if (action == BSP_BTN_DOUBLE && button == BSP_BTN_OK) {
        replay_last_cue();
    }
}

static route_status_code_t process_route_begin(const uint8_t *data, size_t size)
{
    if (size != 9U) return ROUTE_STATUS_INVALID;
    if (route_storage_upload_active()) return ROUTE_STATUS_BUSY;
    (void)route_audio_stop(1000U);
    const uint32_t total = route_wire_u32(data + 1);
    const uint32_t crc = route_wire_u32(data + 5);
    esp_err_t err = route_storage_begin_upload(total, crc);
    return err == ESP_OK ? ROUTE_STATUS_OK :
           err == ESP_ERR_INVALID_SIZE ? ROUTE_STATUS_INVALID : ROUTE_STATUS_STORAGE;
}

static route_status_code_t process_route_chunk(const uint8_t *data, size_t size)
{
    if (size <= 5U || !route_storage_upload_active()) return ROUTE_STATUS_INVALID;
    const uint32_t offset = route_wire_u32(data + 1);
    esp_err_t err = route_storage_write_chunk(offset, data + 5, size - 5U);
    if (err == ESP_OK) return ROUTE_STATUS_OK;
    if (err == ESP_ERR_INVALID_ARG) return ROUTE_STATUS_INVALID;
    return ROUTE_STATUS_STORAGE;
}

static route_status_code_t process_route_commit(void)
{
    if (!route_storage_upload_active()) return ROUTE_STATUS_INVALID;
    /*
     * The active route can still be the source of an audio job. Committing the
     * new route remaps Flash and unloads the old route, so never proceed while
     * the audio worker may still hold a pointer into that mapping.
     */
    if (route_audio_stop(1000U) != ESP_OK) return ROUTE_STATUS_BUSY;
    esp_err_t err = route_storage_commit_upload();
    if (err == ESP_OK) {
        reset_navigation();
        return ROUTE_STATUS_OK;
    }
    if (err == ESP_ERR_INVALID_CRC) return ROUTE_STATUS_CRC;
    if (err == ESP_ERR_INVALID_RESPONSE || err == ESP_ERR_INVALID_SIZE) {
        return ROUTE_STATUS_INVALID;
    }
    return ROUTE_STATUS_STORAGE;
}

static route_status_code_t process_location(const uint8_t *data, size_t size)
{
    if (size != 19U) return ROUTE_STATUS_INVALID;

    const int32_t lat_e7 = route_wire_i32(data + 1);
    const int32_t lon_e7 = route_wire_i32(data + 5);
    const uint16_t accuracy_dm = route_wire_u16(data + 9);
    const uint16_t bearing_deg = route_wire_u16(data + 13);
    if (lat_e7 < -900000000 || lat_e7 > 900000000 ||
        lon_e7 < -1800000000 || lon_e7 > 1800000000 ||
        bearing_deg > 359U) {
        return ROUTE_STATUS_INVALID;
    }

    s_location.lat_e7 = lat_e7;
    s_location.lon_e7 = lon_e7;
    s_location.accuracy_dm = accuracy_dm;
    s_location.speed_cms = route_wire_u16(data + 11);
    s_location.bearing_deg = bearing_deg;
    s_location.phone_timestamp_s = route_wire_u32(data + 15);
    s_last_location_ms = now_ms();
    s_gps_valid = accuracy_dm > 0U && accuracy_dm <= ROUTE_NAV_MAX_ACCURACY_DM;

    /* Keep receiving phone GPS while a replacement route is transferring, but
     * freeze navigation/cue triggering until the atomic slot swap completes. */
    if (route_storage_upload_active()) return ROUTE_STATUS_OK;
    if (!s_nav_ready) return ROUTE_STATUS_OK;

    route_nav_result_t nav_result;
    if (!route_nav_update(&s_nav, &s_location, s_last_location_ms, &nav_result)) {
        return ROUTE_STATUS_INVALID;
    }

    if (nav_result.offroute_started) {
        play_special_cue(ROUTE_CUE_OFF_ROUTE);
    }

    if (nav_result.cue_to_play >= 0) {
        s_last_cue = nav_result.cue_to_play;
        if (s_audio_ready) {
            route_cue_t cue;
            const route_view_t *route = route_storage_view();
            if (route != NULL &&
                route_view_get_cue(route, (uint32_t)nav_result.cue_to_play, &cue)) {
                (void)route_audio_play_cue(route, &cue);
            }
        }
    }
    return ROUTE_STATUS_OK;
}

static route_status_code_t process_control(const uint8_t *data, size_t size)
{
    if (size != 2U) return ROUTE_STATUS_INVALID;
    if (data[1] == ROUTE_CONTROL_REPLAY) {
        replay_last_cue();
        return ROUTE_STATUS_OK;
    }
    if (data[1] == ROUTE_CONTROL_RECENTER) {
        route_ui_recenter(s_nav_ready ? s_nav.route_m : 0U);
        return ROUTE_STATUS_OK;
    }
    return ROUTE_STATUS_UNSUPPORTED;
}

static void process_ble_frame(const app_event_t *event)
{
    const uint8_t *data = event->data.ble.data;
    const size_t size = event->data.ble.size;
    if (size == 0U) return;

    route_status_code_t code;
    switch (data[0]) {
    case ROUTE_MSG_PING:
        code = size == 1U ? ROUTE_STATUS_OK : ROUTE_STATUS_INVALID;
        break;
    case ROUTE_MSG_ROUTE_BEGIN:
        code = process_route_begin(data, size);
        break;
    case ROUTE_MSG_ROUTE_CHUNK:
        code = process_route_chunk(data, size);
        break;
    case ROUTE_MSG_ROUTE_COMMIT:
        code = size == 1U ? process_route_commit() : ROUTE_STATUS_INVALID;
        break;
    case ROUTE_MSG_ROUTE_ABORT:
        if (size == 1U) {
            route_storage_abort_upload();
            code = ROUTE_STATUS_OK;
        } else {
            code = ROUTE_STATUS_INVALID;
        }
        break;
    case ROUTE_MSG_LOCATION:
        code = process_location(data, size);
        break;
    case ROUTE_MSG_CONTROL:
        code = process_control(data, size);
        break;
    default:
        code = ROUTE_STATUS_UNSUPPORTED;
        break;
    }
    send_status(data[0], code);
}

static void render_ui(void)
{
    const uint32_t now = now_ms();
    if (s_gps_valid && s_last_location_ms != 0U &&
        now - s_last_location_ms > APP_GPS_TIMEOUT_MS) {
        s_gps_valid = false;
    }

    route_ui_model_t model = {
        .route = route_storage_view(),
        .ble_connected = s_ble_connected,
        .gps_valid = s_gps_valid,
        .offroute = s_nav_ready && s_nav.offroute,
        .upload_active = route_storage_upload_active(),
        .battery_percent = s_battery_percent,
        .route_m = s_nav_ready ? s_nav.route_m : 0U,
        .cross_track_m = s_nav_ready ? s_nav.cross_track_m : 0.0f,
        .current_lat_e7 = s_location.lat_e7,
        .current_lon_e7 = s_location.lon_e7,
    };
    const uint32_t total = route_storage_upload_total_size();
    if (model.upload_active && total > 0U) {
        uint32_t percent = route_storage_upload_next_offset() * 100U / total;
        model.upload_percent = percent > 100U ? 100U : (uint8_t)percent;
    }
    if (s_nav_ready) {
        model.have_next_cue = route_nav_next_cue(&s_nav, &model.next_cue, NULL);
    }

    if (bsp_lvgl_lock(100)) {
        route_ui_render(&model);
        bsp_lvgl_unlock();
    }
}

static void app_task(void *arg)
{
    (void)arg;
    app_event_t event;
    uint32_t last_render = 0U;
    uint32_t last_battery = 0U;

    for (;;) {
        if (xQueueReceive(s_queue, &event, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (event.type == APP_EVENT_BUTTON) process_button(&event);
            else if (event.type == APP_EVENT_BLE_FRAME) process_ble_frame(&event);
            else if (event.type == APP_EVENT_LINK) {
                s_ble_connected = event.data.link.connected;
                if (!s_ble_connected) s_gps_valid = false;
            }
        }

        const uint32_t now = now_ms();
        if (s_battery_ready && (last_battery == 0U || now - last_battery >= APP_BATTERY_PERIOD_MS)) {
            s_battery_percent = bsp_battery_soc();
            last_battery = now;
        }
        if (last_render == 0U || now - last_render >= APP_RENDER_PERIOD_MS) {
            render_ui();
            last_render = now;
        }
    }
}

esp_err_t route_app_start(void)
{
    s_queue = xQueueCreate(APP_QUEUE_DEPTH, sizeof(app_event_t));
    if (s_queue == NULL) return ESP_ERR_NO_MEM;

    if (bsp_lvgl_lock(1000)) {
        esp_err_t err = route_ui_init();
        bsp_lvgl_unlock();
        if (err != ESP_OK) return err;
    } else {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed without erase: %s", esp_err_to_name(nvs_err));
    }

    if (nvs_err == ESP_OK) {
        esp_err_t storage_err = route_storage_init();
        if (storage_err != ESP_OK) {
            ESP_LOGE(TAG, "route storage init failed: %s", esp_err_to_name(storage_err));
        }
    }
    reset_navigation();

    if (bsp_audio_init() == ESP_OK && route_audio_init() == ESP_OK) {
        s_audio_ready = true;
    } else {
        ESP_LOGW(TAG, "audio unavailable; visual navigation remains active");
    }
    s_battery_ready = bsp_battery_init() == ESP_OK;
    if (!s_battery_ready) s_battery_percent = -1;

    if (xTaskCreate(app_task, "route_app", 6144, NULL, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    s_ready = true;
    esp_err_t button_err = bsp_button_init(on_button, NULL);
    if (button_err != ESP_OK) {
        ESP_LOGE(TAG, "button init failed: %s", esp_err_to_name(button_err));
    }

    if (nvs_err == ESP_OK) {
        route_ble_config_t config = {
            .rx_cb = on_ble_rx,
            .link_cb = on_ble_link,
            .context = NULL,
        };
        esp_err_t ble_err = route_ble_init(&config);
        if (ble_err == ESP_OK) ble_err = route_ble_start();
        if (ble_err != ESP_OK) {
            ESP_LOGE(TAG, "BLE init failed: %s", esp_err_to_name(ble_err));
        }
    }

    return button_err == ESP_OK ? ESP_OK : button_err;
}
