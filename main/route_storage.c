#include "route_storage.h"

#include "esp_log.h"
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "spi_flash_mmap.h"

#include <inttypes.h>
#include <string.h>

#define ROUTE_PARTITION_SUBTYPE 0x40
#define ROUTE_NVS_NAMESPACE "route_meta"
#define ROUTE_NVS_ACTIVE_KEY "active"
#define ROUTE_ERASE_BLOCK 4096U

static const char *TAG = "route_storage";

typedef struct {
    const esp_partition_t *slot[2];
    uint8_t active_slot;
    bool have_active;
    const uint8_t *mapped;
    spi_flash_mmap_handle_t mmap_handle;
    route_view_t view;

    bool upload_active;
    uint8_t upload_slot;
    uint32_t upload_total;
    uint32_t upload_expected_crc;
    uint32_t upload_crc;
    uint32_t upload_offset;
} route_storage_state_t;

static route_storage_state_t s_storage;

static void unload_route(void)
{
    if (s_storage.mapped != NULL) {
        spi_flash_munmap(s_storage.mmap_handle);
        s_storage.mapped = NULL;
        memset(&s_storage.view, 0, sizeof(s_storage.view));
    }
    s_storage.have_active = false;
}

static esp_err_t map_and_validate(uint8_t slot, route_view_t *out_view,
                                  const uint8_t **out_ptr,
                                  spi_flash_mmap_handle_t *out_handle)
{
    if (slot > 1U || s_storage.slot[slot] == NULL) return ESP_ERR_INVALID_ARG;

    const void *mapped = NULL;
    spi_flash_mmap_handle_t handle = 0;
    esp_err_t err = esp_partition_mmap(s_storage.slot[slot], 0, s_storage.slot[slot]->size,
                                       ESP_PARTITION_MMAP_DATA, &mapped, &handle);
    if (err != ESP_OK) return err;

    route_view_t view;
    if (!route_view_init(&view, mapped, s_storage.slot[slot]->size, true)) {
        spi_flash_munmap(handle);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *out_view = view;
    *out_ptr = mapped;
    *out_handle = handle;
    return ESP_OK;
}

static esp_err_t load_slot(uint8_t slot)
{
    route_view_t view;
    const uint8_t *mapped = NULL;
    spi_flash_mmap_handle_t handle = 0;
    esp_err_t err = map_and_validate(slot, &view, &mapped, &handle);
    if (err != ESP_OK) return err;

    unload_route();
    s_storage.view = view;
    s_storage.mapped = mapped;
    s_storage.mmap_handle = handle;
    s_storage.active_slot = slot;
    s_storage.have_active = true;
    ESP_LOGI(TAG, "route loaded slot=%u id=%" PRIu32 " points=%" PRIu32 " cues=%u",
             slot, view.header.route_id, view.header.point_count, view.header.cue_count);
    return ESP_OK;
}

static esp_err_t read_active_slot(uint8_t *slot)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ROUTE_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_u8(handle, ROUTE_NVS_ACTIVE_KEY, slot);
    nvs_close(handle);
    return err;
}

static esp_err_t write_active_slot(uint8_t slot)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(ROUTE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u8(handle, ROUTE_NVS_ACTIVE_KEY, slot);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t route_storage_init(void)
{
    memset(&s_storage, 0, sizeof(s_storage));
    s_storage.slot[0] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 ROUTE_PARTITION_SUBTYPE,
                                                 ROUTE_STORAGE_SLOT_A_LABEL);
    s_storage.slot[1] = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 ROUTE_PARTITION_SUBTYPE,
                                                 ROUTE_STORAGE_SLOT_B_LABEL);
    if (s_storage.slot[0] == NULL || s_storage.slot[1] == NULL) {
        ESP_LOGE(TAG, "route partitions not found");
        return ESP_ERR_NOT_FOUND;
    }

    uint8_t requested = 0;
    esp_err_t pref = read_active_slot(&requested);
    if (pref == ESP_OK && requested <= 1U && load_slot(requested) == ESP_OK) {
        return ESP_OK;
    }

    for (uint8_t slot = 0; slot < 2U; ++slot) {
        if ((pref == ESP_OK && slot == requested) || load_slot(slot) != ESP_OK) continue;
        (void)write_active_slot(slot);
        return ESP_OK;
    }

    ESP_LOGI(TAG, "no valid route stored yet");
    return ESP_OK;
}

const route_view_t *route_storage_view(void)
{
    return s_storage.have_active ? &s_storage.view : NULL;
}

bool route_storage_has_route(void)
{
    return s_storage.have_active;
}

esp_err_t route_storage_begin_upload(uint32_t total_size, uint32_t whole_crc32)
{
    if (s_storage.upload_active) return ESP_ERR_INVALID_STATE;
    if (total_size < ROUTE_FILE_HEADER_SIZE) return ESP_ERR_INVALID_SIZE;

    const uint8_t target = s_storage.have_active ? (uint8_t)(1U - s_storage.active_slot) : 0U;
    if (s_storage.slot[target] == NULL || total_size > s_storage.slot[target]->size) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t erase_size = (total_size + ROUTE_ERASE_BLOCK - 1U) &
                                ~(ROUTE_ERASE_BLOCK - 1U);
    esp_err_t err = esp_partition_erase_range(s_storage.slot[target], 0, erase_size);
    if (err != ESP_OK) return err;

    s_storage.upload_active = true;
    s_storage.upload_slot = target;
    s_storage.upload_total = total_size;
    s_storage.upload_expected_crc = whole_crc32;
    s_storage.upload_crc = 0U;
    s_storage.upload_offset = 0U;
    return ESP_OK;
}

static esp_err_t verify_written_chunk(uint32_t offset, const uint8_t *data, size_t size)
{
    uint8_t existing[64];
    size_t checked = 0U;

    while (checked < size) {
        const size_t remaining = size - checked;
        const size_t chunk = remaining < sizeof(existing) ? remaining : sizeof(existing);
        esp_err_t err = esp_partition_read(s_storage.slot[s_storage.upload_slot],
                                           offset + (uint32_t)checked,
                                           existing, chunk);
        if (err != ESP_OK) return err;
        if (memcmp(existing, data + checked, chunk) != 0) return ESP_ERR_INVALID_ARG;
        checked += chunk;
    }
    return ESP_OK;
}

esp_err_t route_storage_write_chunk(uint32_t offset, const void *data, size_t size)
{
    if (!s_storage.upload_active || data == NULL || size == 0U) return ESP_ERR_INVALID_STATE;
    if (offset > s_storage.upload_total ||
        size > s_storage.upload_total - offset) {
        return ESP_ERR_INVALID_ARG;
    }

    /*
     * A BLE notification carrying the ACK can be lost after the Flash write has
     * already completed. Accept an exact retransmission of bytes that are fully
     * inside the committed upload prefix so the phone can safely retry without
     * corrupting the rolling CRC or advancing the write cursor twice.
     */
    if (offset < s_storage.upload_offset) {
        if (size > s_storage.upload_offset - offset) return ESP_ERR_INVALID_ARG;
        return verify_written_chunk(offset, data, size);
    }
    if (offset != s_storage.upload_offset) return ESP_ERR_INVALID_ARG;

    esp_err_t err = esp_partition_write(s_storage.slot[s_storage.upload_slot],
                                        offset, data, size);
    if (err != ESP_OK) return err;

    s_storage.upload_crc = route_crc32_update(s_storage.upload_crc, data, size);
    s_storage.upload_offset += (uint32_t)size;
    return ESP_OK;
}

esp_err_t route_storage_commit_upload(void)
{
    if (!s_storage.upload_active ||
        s_storage.upload_offset != s_storage.upload_total) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_storage.upload_crc != s_storage.upload_expected_crc) {
        ESP_LOGE(TAG, "upload crc mismatch expected=%08" PRIx32 " actual=%08" PRIx32,
                 s_storage.upload_expected_crc, s_storage.upload_crc);
        route_storage_abort_upload();
        return ESP_ERR_INVALID_CRC;
    }

    route_view_t candidate;
    const uint8_t *mapped = NULL;
    spi_flash_mmap_handle_t handle = 0;
    esp_err_t err = map_and_validate(s_storage.upload_slot, &candidate, &mapped, &handle);
    if (err != ESP_OK) {
        route_storage_abort_upload();
        return err;
    }
    spi_flash_munmap(handle);

    const uint8_t new_slot = s_storage.upload_slot;
    err = write_active_slot(new_slot);
    if (err != ESP_OK) {
        route_storage_abort_upload();
        return err;
    }

    route_storage_abort_upload();
    return load_slot(new_slot);
}

void route_storage_abort_upload(void)
{
    s_storage.upload_active = false;
    s_storage.upload_total = 0U;
    s_storage.upload_expected_crc = 0U;
    s_storage.upload_crc = 0U;
    s_storage.upload_offset = 0U;
}

bool route_storage_upload_active(void)
{
    return s_storage.upload_active;
}

uint32_t route_storage_upload_next_offset(void)
{
    return s_storage.upload_offset;
}

uint32_t route_storage_upload_total_size(void)
{
    return s_storage.upload_total;
}
