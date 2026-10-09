#include "route_ble.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include <string.h>

#define ROUTE_BLE_DEVICE_NAME "CTRA-Passport"
#define ROUTE_BLE_CONN_NONE BLE_HS_CONN_HANDLE_NONE

static const char *TAG = "route_ble";

static const ble_uuid128_t s_service_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0,
    0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e);

static route_ble_config_t s_config;
static bool s_initialized;
static bool s_started;
static volatile uint16_t s_conn_handle = ROUTE_BLE_CONN_NONE;
static volatile bool s_notify_enabled;
static uint16_t s_tx_handle;
static uint8_t s_addr_type;
static uint8_t s_last_status[32];
static size_t s_last_status_size;

static int route_gap_event(struct ble_gap_event *event, void *arg);
static int route_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg);

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = route_gatt_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = route_gatt_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_tx_handle,
            },
            {0},
        },
    },
    {0},
};

static int start_advertising(void)
{
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (const uint8_t *)ROUTE_BLE_DEVICE_NAME;
    fields.name_len = strlen(ROUTE_BLE_DEVICE_NAME);
    fields.name_is_complete = 1;
    fields.uuids128 = &s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) return rc;

    struct ble_gap_adv_params params = {0};
    params.conn_mode = BLE_GAP_CONN_MODE_UND;
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params,
                             route_gap_event, NULL);
}

static void on_reset(int reason)
{
    ESP_LOGE(TAG, "NimBLE reset: %d", reason);
    s_conn_handle = ROUTE_BLE_CONN_NONE;
    s_notify_enabled = false;
    if (s_config.link_cb != NULL) s_config.link_cb(false, s_config.context);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc == 0 && s_started) rc = start_advertising();
    if (rc != 0) ESP_LOGE(TAG, "BLE sync/advertise failed: %d", rc);
}

static int route_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_notify_enabled = false;
            if (s_config.link_cb != NULL) s_config.link_cb(true, s_config.context);
        } else if (s_started) {
            (void)start_advertising();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn_handle = ROUTE_BLE_CONN_NONE;
        s_notify_enabled = false;
        if (s_config.link_cb != NULL) s_config.link_cb(false, s_config.context);
        if (s_started) (void)start_advertising();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_handle) {
            s_notify_enabled = event->subscribe.cur_notify != 0;
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (s_started && s_conn_handle == ROUTE_BLE_CONN_NONE) {
            (void)start_advertising();
        }
        break;
    default:
        break;
    }
    return 0;
}

static int route_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr_handle == s_tx_handle) {
        if (s_last_status_size == 0U) return 0;
        return os_mbuf_append(ctxt->om, s_last_status, s_last_status_size) == 0
            ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }

    const uint16_t packet_size = OS_MBUF_PKTLEN(ctxt->om);
    if (packet_size == 0U || packet_size > ROUTE_BLE_MAX_RX) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }

    uint8_t frame[ROUTE_BLE_MAX_RX];
    uint16_t copied = 0;
    for (struct os_mbuf *mbuf = ctxt->om; mbuf != NULL; mbuf = SLIST_NEXT(mbuf, om_next)) {
        if ((uint32_t)copied + mbuf->om_len > sizeof(frame)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        memcpy(frame + copied, mbuf->om_data, mbuf->om_len);
        copied += mbuf->om_len;
    }

    if (s_config.rx_cb != NULL) {
        s_config.rx_cb(frame, copied, s_config.context);
    }
    return 0;
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    vTaskDelete(NULL);
}

esp_err_t route_ble_init(const route_ble_config_t *config)
{
    if (config == NULL || config->rx_cb == NULL || config->link_cb == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_initialized) return ESP_OK;

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) return err;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_services);
    if (rc == 0) rc = ble_svc_gap_device_name_set(ROUTE_BLE_DEVICE_NAME);
    if (rc != 0) {
        (void)nimble_port_deinit();
        return ESP_FAIL;
    }

    s_config = *config;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    s_initialized = true;
    return ESP_OK;
}

esp_err_t route_ble_start(void)
{
    if (!s_initialized) return ESP_ERR_INVALID_STATE;
    if (s_started) return ESP_OK;
    s_started = true;
    if (xTaskCreatePinnedToCore(host_task, "route_ble", NIMBLE_HS_STACK_SIZE,
                               NULL, configMAX_PRIORITIES - 4, NULL,
                               NIMBLE_CORE) != pdPASS) {
        s_started = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t route_ble_send_status(const uint8_t *data, size_t size)
{
    if (data == NULL || size == 0U || size > sizeof(s_last_status)) {
        return ESP_ERR_INVALID_ARG;
    }
    memcpy(s_last_status, data, size);
    s_last_status_size = size;

    const uint16_t conn = s_conn_handle;
    if (conn == ROUTE_BLE_CONN_NONE || !s_notify_enabled) {
        return ESP_ERR_INVALID_STATE;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, size);
    if (om == NULL) return ESP_ERR_NO_MEM;
    int rc = ble_gatts_notify_custom(conn, s_tx_handle, om);
    return rc == 0 ? ESP_OK : ESP_FAIL;
}

bool route_ble_connected(void)
{
    return s_conn_handle != ROUTE_BLE_CONN_NONE;
}

bool route_ble_notifications_ready(void)
{
    return route_ble_connected() && s_notify_enabled;
}
