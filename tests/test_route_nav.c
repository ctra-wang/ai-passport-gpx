#include "route_format.h"
#include "route_nav.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static route_view_t make_route(uint8_t *buf)
{
    const uint32_t po = ROUTE_FILE_HEADER_SIZE;
    const uint32_t mo = po + 3U * ROUTE_POINT_SIZE;
    const uint32_t co = mo;
    const uint32_t data = co + 2U * ROUTE_CUE_SIZE;
    const uint32_t total = data;

    memset(buf, 0, total);
    memcpy(buf, ROUTE_FILE_MAGIC, 4);
    put16(buf + 4, ROUTE_FILE_VERSION);
    put16(buf + 6, ROUTE_FILE_HEADER_SIZE);
    put32(buf + 8, total);
    put32(buf + 16, 7U);
    put32(buf + 20, 200U);
    put32(buf + 24, 3U);
    put16(buf + 28, 0U);
    put16(buf + 30, 2U);
    put32(buf + 32, po);
    put32(buf + 36, mo);
    put32(buf + 40, co);
    put32(buf + 44, data);
    put32(buf + 48, (uint32_t)(int32_t)-10000);
    put32(buf + 52, 0U);
    put32(buf + 56, 10000U);
    put32(buf + 60, 18000U);

    uint8_t *p = buf + po;
    put32(p + 0, 0U); put32(p + 4, 0U); put32(p + 8, 0U);
    p += ROUTE_POINT_SIZE;
    put32(p + 0, 0U); put32(p + 4, 9000U); put32(p + 8, 100U);
    p += ROUTE_POINT_SIZE;
    put32(p + 0, 0U); put32(p + 4, 18000U); put32(p + 8, 200U);

    p = buf + co;
    put32(p + 0, 0U);
    put16(p + 4, 0U);
    p[6] = ROUTE_CUE_OFF_ROUTE;
    p[7] = 5U;
    put32(p + 8, 0U);
    put32(p + 12, 0U);
    put32(p + 16, 0U);
    put32(p + 20, 0U);
    put16(p + 24, 0U);
    p[26] = ROUTE_AUDIO_NONE;

    p += ROUTE_CUE_SIZE;
    put32(p + 0, 120U);
    put16(p + 4, 40U);
    p[6] = ROUTE_CUE_RIGHT;
    p[7] = 3U;
    put32(p + 8, 0U);
    put32(p + 12, 10800U);
    put32(p + 16, 0U);
    put32(p + 20, 0U);
    put16(p + 24, 0U);
    p[26] = ROUTE_AUDIO_NONE;

    put32(buf + 12, route_crc32(buf + ROUTE_FILE_HEADER_SIZE,
                                total - ROUTE_FILE_HEADER_SIZE));
    route_view_t view;
    assert(route_view_init(&view, buf, total, true));
    return view;
}

int main(void)
{
    uint8_t buf[256];
    route_view_t route = make_route(buf);
    route_nav_t nav;
    route_nav_init(&nav, &route);

    route_cue_t next_cue;
    uint32_t next_index = 0U;
    assert(route_nav_next_cue(&nav, &next_cue, &next_index));
    assert(next_index == 1U);
    assert(next_cue.type == ROUTE_CUE_RIGHT);

    route_location_t loc = {
        .lat_e7 = 0,
        .lon_e7 = 8100,
        .accuracy_dm = 50,
    };
    route_nav_result_t result;
    assert(route_nav_update(&nav, &loc, 1000U, &result));
    assert(result.route_m >= 85U && result.route_m <= 95U);
    assert(result.cross_track_m < 2.0f);
    assert(result.cue_to_play == 1);

    assert(route_nav_update(&nav, &loc, 2000U, &result));
    assert(result.cue_to_play == -1);

    loc.lat_e7 = 6000;
    assert(route_nav_update(&nav, &loc, 3000U, &result));
    assert(!result.offroute_started);
    assert(route_nav_update(&nav, &loc, 14000U, &result));
    assert(result.offroute_started);
    assert(nav.offroute);

    loc.lat_e7 = 0;
    assert(route_nav_update(&nav, &loc, 15000U, &result));
    assert(!result.offroute_cleared);
    assert(route_nav_update(&nav, &loc, 21000U, &result));
    assert(result.offroute_cleared);
    assert(!nav.offroute);

    route_nav_init(&nav, &route);
    loc.lat_e7 = 0;
    loc.lon_e7 = 8100;
    loc.accuracy_dm = 0;
    assert(route_nav_update(&nav, &loc, 22000U, &result));
    assert(result.cue_to_play == -1);
    assert(!nav.offroute);

    puts("route_nav: PASS");
    return 0;
}
