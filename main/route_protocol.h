#pragma once

#include <stdint.h>

#define ROUTE_MSG_PING          0x01U
#define ROUTE_MSG_ROUTE_BEGIN   0x10U
#define ROUTE_MSG_ROUTE_CHUNK   0x11U
#define ROUTE_MSG_ROUTE_COMMIT  0x12U
#define ROUTE_MSG_ROUTE_ABORT   0x13U
#define ROUTE_MSG_LOCATION      0x20U
#define ROUTE_MSG_CONTROL       0x21U

#define ROUTE_MSG_STATUS        0x80U

#define ROUTE_CONTROL_REPLAY    0x01U
#define ROUTE_CONTROL_RECENTER  0x02U

typedef enum {
    ROUTE_STATUS_OK = 0,
    ROUTE_STATUS_INVALID = 1,
    ROUTE_STATUS_BUSY = 2,
    ROUTE_STATUS_CRC = 3,
    ROUTE_STATUS_STORAGE = 4,
    ROUTE_STATUS_UNSUPPORTED = 5,
} route_status_code_t;

#define ROUTE_STATUS_FLAG_HAS_ROUTE  (1U << 0)
#define ROUTE_STATUS_FLAG_UPLOADING  (1U << 1)
#define ROUTE_STATUS_FLAG_GPS_VALID  (1U << 2)
#define ROUTE_STATUS_FLAG_OFF_ROUTE  (1U << 3)
#define ROUTE_STATUS_FLAG_AUDIO      (1U << 4)

static inline uint16_t route_wire_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t route_wire_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline int32_t route_wire_i32(const uint8_t *p)
{
    return (int32_t)route_wire_u32(p);
}

static inline void route_wire_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void route_wire_put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}
