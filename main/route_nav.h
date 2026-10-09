#pragma once

#include "route_format.h"

#include <stdbool.h>
#include <stdint.h>

#define ROUTE_NAV_MAX_ACCURACY_DM 500U
#define ROUTE_NAV_OFFROUTE_ENTER_M 50.0f
#define ROUTE_NAV_OFFROUTE_EXIT_M 30.0f
#define ROUTE_NAV_OFFROUTE_ENTER_MS 10000U
#define ROUTE_NAV_OFFROUTE_EXIT_MS 5000U

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint16_t accuracy_dm;
    uint16_t speed_cms;
    uint16_t bearing_deg;
    uint32_t phone_timestamp_s;
} route_location_t;

typedef struct {
    bool matched;
    uint32_t route_m;
    float cross_track_m;
    uint32_t segment_index;
    int32_t cue_to_play;
    bool offroute_started;
    bool offroute_cleared;
} route_nav_result_t;

typedef struct {
    const route_view_t *route;
    bool have_fix;
    bool offroute;
    uint32_t route_m;
    float cross_track_m;
    uint32_t nearest_segment;
    uint32_t next_cue;
    int32_t last_triggered_cue;
    uint32_t offroute_candidate_since_ms;
    uint32_t recovery_candidate_since_ms;
} route_nav_t;

void route_nav_init(route_nav_t *nav, const route_view_t *route);
bool route_nav_update(route_nav_t *nav, const route_location_t *location,
                      uint32_t now_ms, route_nav_result_t *result);
bool route_nav_next_cue(const route_nav_t *nav, route_cue_t *cue, uint32_t *index_out);
