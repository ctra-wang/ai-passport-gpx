#include "route_format.h"

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

static size_t build_route(uint8_t *buf, size_t cap)
{
    const uint32_t points_offset = ROUTE_FILE_HEADER_SIZE;
    const uint32_t markers_offset = points_offset + 3U * ROUTE_POINT_SIZE;
    const uint32_t cues_offset = markers_offset + ROUTE_MARKER_SIZE;
    const uint32_t data_offset = cues_offset + ROUTE_CUE_SIZE;
    const uint32_t total = data_offset + 6U;
    assert(cap >= total);
    memset(buf, 0, total);

    memcpy(buf, ROUTE_FILE_MAGIC, 4);
    put16(buf + 4, ROUTE_FILE_VERSION);
    put16(buf + 6, ROUTE_FILE_HEADER_SIZE);
    put32(buf + 8, total);
    put32(buf + 16, 42U);
    put32(buf + 20, 200U);
    put32(buf + 24, 3U);
    put16(buf + 28, 1U);
    put16(buf + 30, 1U);
    put32(buf + 32, points_offset);
    put32(buf + 36, markers_offset);
    put32(buf + 40, cues_offset);
    put32(buf + 44, data_offset);
    put32(buf + 48, 0U);
    put32(buf + 52, 0U);
    put32(buf + 56, 1000U);
    put32(buf + 60, 20000U);
    memcpy(buf + 64, "TEST ROUTE", 10);

    uint8_t *p = buf + points_offset;
    put32(p + 0, 0U); put32(p + 4, 0U); put32(p + 8, 0U);
    p += ROUTE_POINT_SIZE;
    put32(p + 0, 0U); put32(p + 4, 9000U); put32(p + 8, 100U);
    p += ROUTE_POINT_SIZE;
    put32(p + 0, 0U); put32(p + 4, 18000U); put32(p + 8, 200U);

    p = buf + markers_offset;
    put32(p + 0, 0U);
    put32(p + 4, 9000U);
    put32(p + 8, 100U);
    p[12] = ROUTE_MARKER_CP;
    put16(p + 14, 1U);

    p = buf + cues_offset;
    put32(p + 0, 120U);
    put16(p + 4, 40U);
    p[6] = ROUTE_CUE_RIGHT;
    p[7] = 3U;
    put32(p + 8, 0U);
    put32(p + 12, 10800U);
    put32(p + 16, data_offset);
    put32(p + 20, 6U);
    put16(p + 24, 8000U);
    p[26] = ROUTE_AUDIO_IMA_ADPCM4;

    buf[data_offset + 0] = 0;
    buf[data_offset + 1] = 0;
    buf[data_offset + 2] = 0;
    buf[data_offset + 3] = 0;
    buf[data_offset + 4] = 0x11;
    buf[data_offset + 5] = 0x22;

    put32(buf + 12, route_crc32(buf + ROUTE_FILE_HEADER_SIZE,
                                total - ROUTE_FILE_HEADER_SIZE));
    return total;
}

int main(void)
{
    uint8_t buf[256];
    const size_t size = build_route(buf, sizeof(buf));
    route_view_t view;
    assert(route_view_init(&view, buf, size, true));
    assert(view.header.route_id == 42U);
    assert(view.header.point_count == 3U);
    assert(strcmp(view.header.name, "TEST ROUTE") == 0);

    route_point_t point;
    assert(route_view_get_point(&view, 1U, &point));
    assert(point.lon_e7 == 9000);
    assert(point.route_m == 100U);

    route_marker_t marker;
    assert(route_view_get_marker(&view, 0U, &marker));
    assert(marker.type == ROUTE_MARKER_CP);
    assert(marker.id == 1U);

    route_cue_t cue;
    assert(route_view_get_cue(&view, 0U, &cue));
    assert(cue.route_m == 120U);
    assert(cue.audio_length == 6U);
    assert(route_view_slice(&view, cue.audio_offset, cue.audio_length) != NULL);
    int32_t dm;
    assert(!route_view_get_elevation(&view, 0, &dm));

    buf[size - 1U] ^= 0xFFU;
    assert(!route_view_init(&view, buf, size, true));

    (void)build_route(buf, sizeof(buf));
    const uint32_t marker_offset = ROUTE_FILE_HEADER_SIZE + 3U * ROUTE_POINT_SIZE;
    buf[marker_offset + 12U] = 0U;
    put32(buf + 12, route_crc32(buf + ROUTE_FILE_HEADER_SIZE,
                                size - ROUTE_FILE_HEADER_SIZE));
    assert(!route_view_init(&view, buf, size, true));

    (void)build_route(buf, sizeof(buf));
    const uint32_t extension = ROUTE_FILE_HEADER_SIZE + 3U * ROUTE_POINT_SIZE;
    memmove(buf + extension + 20, buf + extension, size - extension);
    memcpy(buf + extension, "ELEV", 4);
    put32(buf + extension + 4, 3);
    put32(buf + extension + 8, (uint32_t)-250);
    put32(buf + extension + 12, (uint32_t)ROUTE_ELEVATION_UNKNOWN);
    put32(buf + extension + 16, 1250);
    put32(buf + 8, (uint32_t)size + 20);
    put32(buf + 36, extension + 20);
    put32(buf + 40, extension + 20 + ROUTE_MARKER_SIZE);
    const uint32_t new_data = extension + 20 + ROUTE_MARKER_SIZE + ROUTE_CUE_SIZE;
    put32(buf + 44, new_data);
    put32(buf + extension + 20 + ROUTE_MARKER_SIZE + 16, new_data);
    put32(buf + 12, route_crc32(buf + ROUTE_FILE_HEADER_SIZE, size + 20 - ROUTE_FILE_HEADER_SIZE));
    assert(route_view_init(&view, buf, size + 20, true));
    assert(route_view_get_elevation(&view, 0, &dm) && dm == -250);
    assert(!route_view_get_elevation(&view, 1, &dm));
    assert(route_view_get_elevation(&view, 2, &dm) && dm == 1250);
    assert(!route_view_get_elevation(&view, 3, &dm));
    assert(route_view_get_cue(&view, 0, &cue) && cue.audio_offset == new_data);
    put32(buf + extension + 4, 4);
    assert(!route_view_init(&view, buf, size + 20, false));
    put32(buf + extension + 4, 3);
    put32(buf + extension + 8, 100001);
    assert(!route_view_init(&view, buf, size + 20, false));

    puts("route_format: PASS");
    return 0;
}
