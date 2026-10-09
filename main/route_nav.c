#include "route_nav.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define METERS_PER_E7_LAT 0.011132f
#define SEARCH_WINDOW_POINTS 120U
#define SEARCH_FALLBACK_DISTANCE_M 120.0f

typedef struct {
    float distance_sq;
    uint32_t segment;
    float t;
} route_match_t;

static float lon_m_per_e7(int32_t lat_e7)
{
    const float lat_rad = (float)lat_e7 * 1.0e-7f * 0.017453292519943295f;
    float value = METERS_PER_E7_LAT * cosf(lat_rad);
    if (value < 0.001f) value = 0.001f;
    return value;
}

static bool segment_match(const route_view_t *route, uint32_t segment,
                          const route_location_t *location, float lon_scale,
                          route_match_t *match)
{
    route_point_t a;
    route_point_t b;
    if (!route_view_get_point(route, segment, &a) ||
        !route_view_get_point(route, segment + 1U, &b)) {
        return false;
    }

    const float ax = (float)(a.lon_e7 - location->lon_e7) * lon_scale;
    const float ay = (float)(a.lat_e7 - location->lat_e7) * METERS_PER_E7_LAT;
    const float bx = (float)(b.lon_e7 - location->lon_e7) * lon_scale;
    const float by = (float)(b.lat_e7 - location->lat_e7) * METERS_PER_E7_LAT;
    const float dx = bx - ax;
    const float dy = by - ay;
    const float len_sq = dx * dx + dy * dy;

    float t = 0.0f;
    if (len_sq > 0.01f) {
        t = -(ax * dx + ay * dy) / len_sq;
        if (t < 0.0f) t = 0.0f;
        if (t > 1.0f) t = 1.0f;
    }
    const float px = ax + t * dx;
    const float py = ay + t * dy;
    const float distance_sq = px * px + py * py;

    if (distance_sq < match->distance_sq) {
        match->distance_sq = distance_sq;
        match->segment = segment;
        match->t = t;
    }
    return true;
}

static bool find_nearest(const route_nav_t *nav, const route_location_t *location,
                         route_match_t *match)
{
    const route_view_t *route = nav->route;
    if (route == NULL || route->header.point_count < 2U) return false;

    match->distance_sq = INFINITY;
    match->segment = 0U;
    match->t = 0.0f;
    const float lon_scale = lon_m_per_e7(location->lat_e7);
    const uint32_t last_segment = route->header.point_count - 2U;

    if (nav->have_fix) {
        const uint32_t start = nav->nearest_segment > SEARCH_WINDOW_POINTS
            ? nav->nearest_segment - SEARCH_WINDOW_POINTS : 0U;
        uint32_t end = nav->nearest_segment + SEARCH_WINDOW_POINTS;
        if (end > last_segment) end = last_segment;
        for (uint32_t i = start; i <= end; ++i) {
            (void)segment_match(route, i, location, lon_scale, match);
        }
        if (match->distance_sq <= SEARCH_FALLBACK_DISTANCE_M * SEARCH_FALLBACK_DISTANCE_M) {
            return isfinite(match->distance_sq);
        }
        match->distance_sq = INFINITY;
    }

    for (uint32_t i = 0; i <= last_segment; ++i) {
        (void)segment_match(route, i, location, lon_scale, match);
    }
    return isfinite(match->distance_sq);
}

void route_nav_init(route_nav_t *nav, const route_view_t *route)
{
    if (nav == NULL) return;
    memset(nav, 0, sizeof(*nav));
    nav->route = route;
    nav->last_triggered_cue = -1;
}

static uint32_t interpolated_route_m(const route_view_t *route, const route_match_t *match)
{
    route_point_t a;
    route_point_t b;
    if (!route_view_get_point(route, match->segment, &a) ||
        !route_view_get_point(route, match->segment + 1U, &b)) {
        return 0U;
    }
    const float delta = (float)(b.route_m - a.route_m) * match->t;
    return a.route_m + (uint32_t)(delta + 0.5f);
}

static void update_offroute(route_nav_t *nav, uint32_t now_ms, route_nav_result_t *result)
{
    if (!nav->offroute) {
        nav->recovery_candidate_since_ms = 0U;
        if (nav->cross_track_m > ROUTE_NAV_OFFROUTE_ENTER_M) {
            if (nav->offroute_candidate_since_ms == 0U) {
                nav->offroute_candidate_since_ms = now_ms == 0U ? 1U : now_ms;
            } else if (now_ms - nav->offroute_candidate_since_ms >= ROUTE_NAV_OFFROUTE_ENTER_MS) {
                nav->offroute = true;
                nav->offroute_candidate_since_ms = 0U;
                result->offroute_started = true;
            }
        } else {
            nav->offroute_candidate_since_ms = 0U;
        }
        return;
    }

    nav->offroute_candidate_since_ms = 0U;
    if (nav->cross_track_m < ROUTE_NAV_OFFROUTE_EXIT_M) {
        if (nav->recovery_candidate_since_ms == 0U) {
            nav->recovery_candidate_since_ms = now_ms == 0U ? 1U : now_ms;
        } else if (now_ms - nav->recovery_candidate_since_ms >= ROUTE_NAV_OFFROUTE_EXIT_MS) {
            nav->offroute = false;
            nav->recovery_candidate_since_ms = 0U;
            result->offroute_cleared = true;
        }
    } else {
        nav->recovery_candidate_since_ms = 0U;
    }
}

static int32_t maybe_trigger_cue(route_nav_t *nav, const route_location_t *location)
{
    const route_view_t *route = nav->route;
    while (nav->next_cue < route->header.cue_count) {
        route_cue_t cue;
        if (!route_view_get_cue(route, nav->next_cue, &cue)) return -1;

        if (cue.type == ROUTE_CUE_OFF_ROUTE) {
            ++nav->next_cue;
            continue;
        }

        const int32_t delta = (int32_t)cue.route_m - (int32_t)nav->route_m;
        if (delta < -25) {
            ++nav->next_cue;
            continue;
        }

        if (location->accuracy_dm > 0U &&
            location->accuracy_dm <= ROUTE_NAV_MAX_ACCURACY_DM &&
            nav->cross_track_m <= ROUTE_NAV_OFFROUTE_ENTER_M &&
            delta <= (int32_t)cue.trigger_before_m &&
            delta >= -15 &&
            nav->last_triggered_cue != (int32_t)nav->next_cue) {
            nav->last_triggered_cue = (int32_t)nav->next_cue;
            return (int32_t)nav->next_cue;
        }
        break;
    }
    return -1;
}

bool route_nav_update(route_nav_t *nav, const route_location_t *location,
                      uint32_t now_ms, route_nav_result_t *result)
{
    if (nav == NULL || location == NULL || result == NULL ||
        nav->route == NULL || nav->route->header.point_count < 2U) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->cue_to_play = -1;

    route_match_t match;
    if (!find_nearest(nav, location, &match)) return false;

    nav->have_fix = true;
    nav->nearest_segment = match.segment;
    nav->route_m = interpolated_route_m(nav->route, &match);
    nav->cross_track_m = sqrtf(match.distance_sq);

    if (location->accuracy_dm > 0U &&
        location->accuracy_dm <= ROUTE_NAV_MAX_ACCURACY_DM) {
        update_offroute(nav, now_ms, result);
    } else {
        nav->offroute_candidate_since_ms = 0U;
        nav->recovery_candidate_since_ms = 0U;
    }
    result->cue_to_play = maybe_trigger_cue(nav, location);

    result->matched = true;
    result->route_m = nav->route_m;
    result->cross_track_m = nav->cross_track_m;
    result->segment_index = nav->nearest_segment;
    return true;
}

bool route_nav_next_cue(const route_nav_t *nav, route_cue_t *cue, uint32_t *index_out)
{
    if (nav == NULL || nav->route == NULL || cue == NULL) return false;

    for (uint32_t index = nav->next_cue; index < nav->route->header.cue_count; ++index) {
        route_cue_t candidate;
        if (!route_view_get_cue(nav->route, index, &candidate)) return false;
        if (candidate.type == ROUTE_CUE_OFF_ROUTE) continue;
        *cue = candidate;
        if (index_out != NULL) *index_out = index;
        return true;
    }
    return false;
}
