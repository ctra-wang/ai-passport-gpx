#include "route_format.h"

#include <string.h>

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t read_le_i32(const uint8_t *p)
{
    return (int32_t)read_le32(p);
}

uint32_t route_crc32_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *bytes = data;
    uint32_t value = ~crc;

    for (size_t i = 0; i < size; ++i) {
        value ^= bytes[i];
        for (int bit = 0; bit < 8; ++bit) {
            value = (value >> 1) ^ (0xEDB88320U & (uint32_t)-(int32_t)(value & 1U));
        }
    }
    return ~value;
}

uint32_t route_crc32(const void *data, size_t size)
{
    return route_crc32_update(0, data, size);
}

static bool range_valid(uint32_t offset, uint32_t bytes, uint32_t total)
{
    return offset <= total && bytes <= total - offset;
}

static bool coordinate_valid(int32_t lat_e7, int32_t lon_e7)
{
    return lat_e7 >= -900000000 && lat_e7 <= 900000000 &&
           lon_e7 >= -1800000000 && lon_e7 <= 1800000000;
}

static bool decode_header(route_header_t *header, const uint8_t *data, size_t size)
{
    if (size < ROUTE_FILE_HEADER_SIZE || memcmp(data, ROUTE_FILE_MAGIC, 4) != 0) {
        return false;
    }

    memset(header, 0, sizeof(*header));
    header->version = read_le16(data + 4);
    header->header_size = read_le16(data + 6);
    header->total_size = read_le32(data + 8);
    header->payload_crc32 = read_le32(data + 12);
    header->route_id = read_le32(data + 16);
    header->route_distance_m = read_le32(data + 20);
    header->point_count = read_le32(data + 24);
    header->marker_count = read_le16(data + 28);
    header->cue_count = read_le16(data + 30);
    header->points_offset = read_le32(data + 32);
    header->markers_offset = read_le32(data + 36);
    header->cues_offset = read_le32(data + 40);
    header->data_offset = read_le32(data + 44);
    header->min_lat_e7 = read_le_i32(data + 48);
    header->min_lon_e7 = read_le_i32(data + 52);
    header->max_lat_e7 = read_le_i32(data + 56);
    header->max_lon_e7 = read_le_i32(data + 60);
    memcpy(header->name, data + 64, 32);
    header->name[32] = '\0';

    return true;
}

bool route_view_init(route_view_t *view, const void *data_ptr, size_t size, bool verify_crc)
{
    const uint8_t *data = data_ptr;
    route_header_t header;

    if (view == NULL || data == NULL || !decode_header(&header, data, size)) {
        return false;
    }
    if (header.version != ROUTE_FILE_VERSION ||
        header.header_size != ROUTE_FILE_HEADER_SIZE ||
        header.total_size < header.header_size ||
        header.total_size > size ||
        header.point_count < 2U ||
        header.point_count > ROUTE_MAX_POINTS ||
        header.marker_count > ROUTE_MAX_MARKERS ||
        header.cue_count > ROUTE_MAX_CUES ||
        header.min_lat_e7 > header.max_lat_e7 ||
        header.min_lon_e7 > header.max_lon_e7 ||
        !coordinate_valid(header.min_lat_e7, header.min_lon_e7) ||
        !coordinate_valid(header.max_lat_e7, header.max_lon_e7)) {
        return false;
    }

    const uint32_t point_bytes = header.point_count * ROUTE_POINT_SIZE;
    const uint32_t marker_bytes = (uint32_t)header.marker_count * ROUTE_MARKER_SIZE;
    const uint32_t cue_bytes = (uint32_t)header.cue_count * ROUTE_CUE_SIZE;

    if (header.points_offset < header.header_size ||
        !range_valid(header.points_offset, point_bytes, header.total_size) ||
        header.markers_offset < header.points_offset + point_bytes ||
        !range_valid(header.markers_offset, marker_bytes, header.total_size) ||
        header.cues_offset < header.markers_offset + marker_bytes ||
        !range_valid(header.cues_offset, cue_bytes, header.total_size) ||
        header.data_offset < header.cues_offset + cue_bytes ||
        header.data_offset > header.total_size) {
        return false;
    }

    const uint32_t extension_offset = header.points_offset + point_bytes;
    const uint32_t extension_size = header.markers_offset - extension_offset;
    uint32_t elevation_offset = 0;
    if (extension_size >= 4U && memcmp(data + extension_offset, "ELEV", 4) == 0) {
        if (extension_size != 8U + header.point_count * 4U ||
            read_le32(data + extension_offset + 4) != header.point_count) return false;
        elevation_offset = extension_offset + 8U;
        for (uint32_t i = 0; i < header.point_count; ++i) {
            const int32_t dm = read_le_i32(data + elevation_offset + i * 4U);
            if (dm != ROUTE_ELEVATION_UNKNOWN && (dm < -120000 || dm > 100000)) return false;
        }
    }

    if (verify_crc) {
        const uint8_t *payload = data + header.header_size;
        const size_t payload_size = header.total_size - header.header_size;
        if (route_crc32(payload, payload_size) != header.payload_crc32) {
            return false;
        }
    }

    uint32_t last_m = 0;
    for (uint32_t i = 0; i < header.point_count; ++i) {
        const uint8_t *p = data + header.points_offset + i * ROUTE_POINT_SIZE;
        const uint32_t route_m = read_le32(p + 8);
        if ((i > 0U && route_m < last_m) ||
            read_le_i32(p) < header.min_lat_e7 ||
            read_le_i32(p) > header.max_lat_e7 ||
            read_le_i32(p + 4) < header.min_lon_e7 ||
            read_le_i32(p + 4) > header.max_lon_e7) {
            return false;
        }
        last_m = route_m;
    }
    if (last_m > header.route_distance_m + 10U) {
        return false;
    }

    last_m = 0;
    for (uint32_t i = 0; i < header.marker_count; ++i) {
        const uint8_t *p = data + header.markers_offset + i * ROUTE_MARKER_SIZE;
        const int32_t lat_e7 = read_le_i32(p);
        const int32_t lon_e7 = read_le_i32(p + 4);
        const uint32_t route_m = read_le32(p + 8);
        const uint8_t type = p[12];
        if ((i > 0U && route_m < last_m) ||
            route_m > header.route_distance_m + 100U ||
            !coordinate_valid(lat_e7, lon_e7) ||
            type < ROUTE_MARKER_START || type > ROUTE_MARKER_POI) {
            return false;
        }
        last_m = route_m;
    }

    last_m = 0;
    for (uint32_t i = 0; i < header.cue_count; ++i) {
        const uint8_t *p = data + header.cues_offset + i * ROUTE_CUE_SIZE;
        const uint32_t route_m = read_le32(p);
        const int32_t lat_e7 = read_le_i32(p + 8);
        const int32_t lon_e7 = read_le_i32(p + 12);
        const uint32_t audio_offset = read_le32(p + 16);
        const uint32_t audio_length = read_le32(p + 20);
        const uint16_t sample_rate_hz = read_le16(p + 24);
        const uint8_t codec = p[26];
        const uint8_t type = p[6];
        if ((i > 0U && route_m < last_m) ||
            route_m > header.route_distance_m + 100U ||
            !coordinate_valid(lat_e7, lon_e7) ||
            type < ROUTE_CUE_LEFT || type > ROUTE_CUE_OFF_ROUTE) {
            return false;
        }
        if (audio_length == 0U) {
            if (codec != ROUTE_AUDIO_NONE || sample_rate_hz != 0U) return false;
        } else if (codec != ROUTE_AUDIO_IMA_ADPCM4 ||
                   sample_rate_hz < 4000U || sample_rate_hz > 48000U ||
                   audio_offset < header.data_offset ||
                   !range_valid(audio_offset, audio_length, header.total_size) ||
                   audio_length < 4U) {
            return false;
        }
        last_m = route_m;
    }

    view->data = data;
    view->data_size = header.total_size;
    view->header = header;
    view->elevation_offset = elevation_offset;
    return true;
}

bool route_view_get_elevation(const route_view_t *view, uint32_t index, int32_t *decimeters)
{
    if (!view || !decimeters || !view->elevation_offset || index >= view->header.point_count) return false;
    *decimeters = read_le_i32(view->data + view->elevation_offset + index * 4U);
    return *decimeters != ROUTE_ELEVATION_UNKNOWN;
}

bool route_view_get_point(const route_view_t *view, uint32_t index, route_point_t *out)
{
    if (view == NULL || out == NULL || index >= view->header.point_count) return false;
    const uint8_t *p = view->data + view->header.points_offset + index * ROUTE_POINT_SIZE;
    out->lat_e7 = read_le_i32(p);
    out->lon_e7 = read_le_i32(p + 4);
    out->route_m = read_le32(p + 8);
    return true;
}

bool route_view_get_marker(const route_view_t *view, uint32_t index, route_marker_t *out)
{
    if (view == NULL || out == NULL || index >= view->header.marker_count) return false;
    const uint8_t *p = view->data + view->header.markers_offset + index * ROUTE_MARKER_SIZE;
    out->lat_e7 = read_le_i32(p);
    out->lon_e7 = read_le_i32(p + 4);
    out->route_m = read_le32(p + 8);
    out->type = p[12];
    out->flags = p[13];
    out->id = read_le16(p + 14);
    return true;
}

bool route_view_get_cue(const route_view_t *view, uint32_t index, route_cue_t *out)
{
    if (view == NULL || out == NULL || index >= view->header.cue_count) return false;
    const uint8_t *p = view->data + view->header.cues_offset + index * ROUTE_CUE_SIZE;
    out->route_m = read_le32(p);
    out->trigger_before_m = read_le16(p + 4);
    out->type = p[6];
    out->priority = p[7];
    out->lat_e7 = read_le_i32(p + 8);
    out->lon_e7 = read_le_i32(p + 12);
    out->audio_offset = read_le32(p + 16);
    out->audio_length = read_le32(p + 20);
    out->sample_rate_hz = read_le16(p + 24);
    out->audio_codec = p[26];
    out->flags = p[27];
    return true;
}

const uint8_t *route_view_slice(const route_view_t *view, uint32_t offset, uint32_t length)
{
    if (view == NULL || !range_valid(offset, length, (uint32_t)view->data_size)) return NULL;
    return view->data + offset;
}
