#pragma once

#include <stddef.h>
#include <stdint.h>

void route_ui_title(const char *name, char out[28]);
/* Packed I2 pixels, excluding the LVGL palette. Coordinates may be off-screen. */
void route_ui_plot_line(uint8_t *pixels, int width, int height, size_t stride,
                        int x0, int y0, int x1, int y1, uint8_t color);
