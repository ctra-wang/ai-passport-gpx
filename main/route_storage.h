#pragma once

#include "esp_err.h"
#include "route_format.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ROUTE_STORAGE_SLOT_A_LABEL "route_a"
#define ROUTE_STORAGE_SLOT_B_LABEL "route_b"

esp_err_t route_storage_init(void);
const route_view_t *route_storage_view(void);
bool route_storage_has_route(void);

esp_err_t route_storage_begin_upload(uint32_t total_size, uint32_t whole_crc32);
esp_err_t route_storage_write_chunk(uint32_t offset, const void *data, size_t size);
esp_err_t route_storage_commit_upload(void);
void route_storage_abort_upload(void);

bool route_storage_upload_active(void);
uint32_t route_storage_upload_next_offset(void);
uint32_t route_storage_upload_total_size(void);
