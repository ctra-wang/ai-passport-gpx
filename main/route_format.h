#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUTE_FILE_MAGIC "FAPR"
#define ROUTE_FILE_VERSION 1U
#define ROUTE_FILE_HEADER_SIZE 96U
#define ROUTE_POINT_SIZE 12U
#define ROUTE_MARKER_SIZE 16U
#define ROUTE_CUE_SIZE 28U
#define ROUTE_ELEVATION_UNKNOWN INT32_MIN

#define ROUTE_MAX_POINTS 20000U
#define ROUTE_MAX_MARKERS 1024U
#define ROUTE_MAX_CUES 2048U

typedef enum {
    ROUTE_MARKER_START = 1,
    ROUTE_MARKER_FINISH = 2,
    ROUTE_MARKER_CP = 3,
    ROUTE_MARKER_KM = 4,
    ROUTE_MARKER_POI = 5,
} route_marker_type_t;

typedef enum {
    ROUTE_CUE_LEFT = 1,
    ROUTE_CUE_RIGHT = 2,
    ROUTE_CUE_FORK = 3,
    ROUTE_CUE_CP = 4,
    ROUTE_CUE_CLIMB = 5,
    ROUTE_CUE_DESCENT = 6,
    ROUTE_CUE_DANGER = 7,
    ROUTE_CUE_SUPPLY = 8,
    ROUTE_CUE_FINISH = 9,
    ROUTE_CUE_OFF_ROUTE = 10,
} route_cue_type_t;

typedef enum {
    ROUTE_AUDIO_NONE = 0,
    ROUTE_AUDIO_IMA_ADPCM4 = 1,
} route_audio_codec_t;

typedef struct {
    uint16_t version;
    uint16_t header_size;
    uint32_t total_size;
    uint32_t payload_crc32;
    uint32_t route_id;
    uint32_t route_distance_m;
    uint32_t point_count;
    uint16_t marker_count;
    uint16_t cue_count;
    uint32_t points_offset;
    uint32_t markers_offset;
    uint32_t cues_offset;
    uint32_t data_offset;
    int32_t min_lat_e7;
    int32_t min_lon_e7;
    int32_t max_lat_e7;
    int32_t max_lon_e7;
    char name[33];
} route_header_t;

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint32_t route_m;
} route_point_t;

typedef struct {
    int32_t lat_e7;
    int32_t lon_e7;
    uint32_t route_m;
    uint8_t type;
    uint8_t flags;
    uint16_t id;
} route_marker_t;

typedef struct {
    uint32_t route_m;
    uint16_t trigger_before_m;
    uint8_t type;
    uint8_t priority;
    int32_t lat_e7;
    int32_t lon_e7;
    uint32_t audio_offset;
    uint32_t audio_length;
    uint16_t sample_rate_hz;
    uint8_t audio_codec;
    uint8_t flags;
} route_cue_t;

typedef struct {
    const uint8_t *data;
    size_t data_size;
    route_header_t header;
    uint32_t elevation_offset;
} route_view_t;

uint32_t route_crc32_update(uint32_t crc, const void *data, size_t size);
uint32_t route_crc32(const void *data, size_t size);

bool route_view_init(route_view_t *view, const void *data, size_t size, bool verify_crc);
bool route_view_get_point(const route_view_t *view, uint32_t index, route_point_t *out);
bool route_view_get_elevation(const route_view_t *view, uint32_t index, int32_t *decimeters);
bool route_view_get_marker(const route_view_t *view, uint32_t index, route_marker_t *out);
bool route_view_get_cue(const route_view_t *view, uint32_t index, route_cue_t *out);
const uint8_t *route_view_slice(const route_view_t *view, uint32_t offset, uint32_t length);
