#pragma once

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUTE_BLE_MAX_RX 244U

typedef void (*route_ble_rx_cb_t)(const uint8_t *data, size_t size, void *context);
typedef void (*route_ble_link_cb_t)(bool connected, void *context);

typedef struct {
    route_ble_rx_cb_t rx_cb;
    route_ble_link_cb_t link_cb;
    void *context;
} route_ble_config_t;

esp_err_t route_ble_init(const route_ble_config_t *config);
esp_err_t route_ble_start(void);
esp_err_t route_ble_send_status(const uint8_t *data, size_t size);
bool route_ble_connected(void);
bool route_ble_notifications_ready(void);
