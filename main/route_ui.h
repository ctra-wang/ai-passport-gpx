#pragma once

#include "route_format.h"
#include "route_nav.h"

#include "esp_err.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const route_view_t *route;
    bool ble_connected;
    bool gps_valid;
    bool offroute;
    bool upload_active;
    uint8_t upload_percent;
    int battery_percent;
    uint32_t route_m;
    float cross_track_m;
    int32_t current_lat_e7;
    int32_t current_lon_e7;
    bool have_next_cue;
    route_cue_t next_cue;
} route_ui_model_t;

esp_err_t route_ui_init(void);
void route_ui_render(const route_ui_model_t *model);
void route_ui_pan(int direction);
void route_ui_zoom(int direction);
void route_ui_recenter(uint32_t route_m);
bool route_ui_following(void);
void route_ui_toggle_panel(void);
