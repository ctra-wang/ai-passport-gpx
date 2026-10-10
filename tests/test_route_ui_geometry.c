#include "route_ui_geometry.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint8_t pixels[32 * 32 / 4];
static unsigned pixel(int x, int y) { return (pixels[y * 8 + x / 4] >> (6 - 2 * (x & 3))) & 3; }

int main(void)
{
    char text[28];
    route_ui_title("崇州市绿道百公里挑战赛", text);
    assert(strcmp(text, "崇州市绿道百...") == 0);
    route_ui_title("六字符测试名", text);
    assert(strcmp(text, "六字符测试名") == 0);
    route_ui_title("2026年“凡跑”", text);
    assert(strcmp(text, "2026年“...") == 0);
    route_ui_title("短名", text); assert(strcmp(text, "短名") == 0);
    route_ui_title("", text); assert(!*text);
    route_ui_title("\xe4\xb8", text); assert(strcmp(text, "??") == 0);

    route_ui_plot_line(pixels, 32, 32, 8, -1000000, 16, 1000000, 16, 2);
    for (int x = 0; x < 32; ++x) assert(pixel(x, 16) == 2);
    route_ui_plot_line(pixels, 32, 32, 8, 16, -1000000, 16, 1000000, 1);
    assert(pixel(16, 16) == 2 && pixel(16, 3) == 1);
    memset(pixels, 0, sizeof(pixels));
    /* A route exits on the right and returns below; do not join interior points. */
    route_ui_plot_line(pixels, 32, 32, 8, 5, 5, 80, 5, 2);
    route_ui_plot_line(pixels, 32, 32, 8, 80, 5, 80, 25, 2);
    route_ui_plot_line(pixels, 32, 32, 8, 80, 25, 5, 25, 2);
    assert(pixel(16, 5) == 2 && pixel(16, 25) == 2);
    assert(pixel(5, 16) == 0 && pixel(16, 16) == 0);
    for (int i = 0; i < 20000; ++i) {
        route_ui_plot_line(pixels, 32, 32, 8, -40, -40, -20, -20, 1);
    }
    assert(pixel(16, 16) == 0);
    route_ui_plot_line(pixels, 32, 32, 8, 0, 31, 31, 0, 2);
    assert(pixel(0, 31) && pixel(31, 0) && pixel(15, 16));
    puts("Route title and clipped raster geometry: PASS");
}
