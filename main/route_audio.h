#pragma once

#include "esp_err.h"
#include "route_format.h"

#include <stdbool.h>
#include <stdint.h>

esp_err_t route_audio_init(void);
esp_err_t route_audio_play_cue(const route_view_t *route, const route_cue_t *cue);
esp_err_t route_audio_stop(uint32_t timeout_ms);
bool route_audio_is_playing(void);
