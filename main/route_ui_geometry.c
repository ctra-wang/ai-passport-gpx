#include "route_ui_geometry.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

void route_ui_title(const char *name, char out[28])
{
    size_t bytes = 0;
    unsigned count = 0;
    while (*name && count < 6) {
        const unsigned char lead = (unsigned char)*name;
        size_t n = lead < 0x80 ? 1 : lead >= 0xc2 && lead <= 0xdf ? 2 :
                   lead >= 0xe0 && lead <= 0xef ? 3 : lead >= 0xf0 && lead <= 0xf4 ? 4 : 0;
        uint32_t cp = n == 1 ? lead : n == 2 ? lead & 0x1f : n == 3 ? lead & 0x0f : lead & 7;
        for (size_t i = 1; i < n; ++i) {
            if (((unsigned char)name[i] & 0xc0) != 0x80) { n = 0; break; }
            cp = (cp << 6) | (name[i] & 0x3f);
        }
        if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) ||
            (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
            (cp >= 0xd800 && cp <= 0xdfff) || cp < 0x20) n = 0;
        if (n == 0) { out[bytes++] = '?'; ++name; }
        else { memcpy(out + bytes, name, n); bytes += n; name += n; }
        ++count;
    }
    if (*name) { memcpy(out + bytes, "...", 3); bytes += 3; }
    out[bytes] = '\0';
}

void route_ui_plot_line(uint8_t *pixels, int width, int height, size_t stride,
                        int x0, int y0, int x1, int y1, uint8_t color)
{
    if (!pixels || width <= 0 || height <= 0 || stride < (size_t)(width + 3) / 4) return;
    /* Clip each original segment; dropping outside points creates false bridges. */
    const double dx = (double)x1 - x0, dy = (double)y1 - y0;
    const double p[] = {-dx, dx, -dy, dy};
    const double q[] = {x0, width - 1.0 - x0, y0, height - 1.0 - y0};
    double lo = 0, hi = 1;
    for (unsigned i = 0; i < 4; ++i) {
        if (p[i] == 0) { if (q[i] < 0) return; continue; }
        const double t = q[i] / p[i];
        if (p[i] < 0) { if (t > lo) lo = t; }
        else if (t < hi) hi = t;
        if (lo > hi) return;
    }
    x1 = (int)lround(x0 + hi * dx); y1 = (int)lround(y0 + hi * dy);
    x0 = (int)lround(x0 + lo * dx); y0 = (int)lround(y0 + lo * dy);
    const int ax = abs(x1 - x0), ay = -abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int error = ax + ay;
    for (;;) {
        for (int y = y0 - 1; y <= y0 + 1; ++y) {
            for (int x = x0 - 1; x <= x0 + 1; ++x) {
                if (x < 0 || y < 0 || x >= width || y >= height) continue;
                uint8_t *pixel = pixels + y * stride + x / 4;
                const unsigned shift = 6 - 2 * (x & 3);
                if (((*pixel >> shift) & 3U) > (color & 3U)) continue;
                *pixel = (*pixel & ~(3U << shift)) | ((color & 3U) << shift);
            }
        }
        if (x0 == x1 && y0 == y1) break;
        const int twice = 2 * error;
        if (twice >= ay) { error += ay; x0 += sx; }
        if (twice <= ax) { error += ax; y0 += sy; }
    }
}
