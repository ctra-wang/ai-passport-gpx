/* Headless acceptance against the pinned LVGL, with the device's 24 KB pool. */
#include "../main/route_ui.c"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>

static uint16_t framebuffer[240 * 320];
static uint16_t flush_buffer[240 * 20];

static void put32(uint8_t *p, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static uint8_t *fixture(size_t *size)
{
    const unsigned count = 1201, extension = 96 + count * 12;
    const unsigned markers = extension + 8 + count * 4;
    *size = markers + 20 * 16;
    uint8_t *data = calloc(1, *size); assert(data);
    memcpy(data, "FAPR", 4); data[4] = 1; data[6] = 96;
    put32(data + 8, (uint32_t)*size); put32(data + 20, (count - 1) * 8);
    put32(data + 24, count); put32(data + 32, 96);
    data[28] = 20; put32(data + 36, markers);
    for (int i = 40; i <= 44; i += 4) put32(data + i, (uint32_t)*size);
    put32(data + 48, 299960000); put32(data + 52, 1039960000);
    put32(data + 56, 300040000); put32(data + 60, 1040040000);
    memcpy(data + 64, "崇州市绿道百公里挑战", 30);
    memcpy(data + extension, "ELEV", 4); put32(data + extension + 4, count);
    for (unsigned i = 0; i < count; ++i) {
        put32(data + 96 + i * 12, (uint32_t)lround(300000000 + sin(i * 3.14159265 / 200) * 40000));
        put32(data + 100 + i * 12, (uint32_t)lround(1040000000 + sin(i * 3.14159265 / 100) * 40000));
        put32(data + 104 + i * 12, i * 8);
        put32(data + extension + 8 + i * 4, (uint32_t)lround(4200 + sin(i * 3.14159265 / 90) * 800));
    }
    for (unsigned i = 0; i < 20; ++i) {
        uint8_t *p = data + markers + i * 16;
        put32(p, 300070000 - (i / 5) * 18500);
        put32(p + 4, 1039981700 + (i % 5) * 27368);
        put32(p + 8, i * 100); p[12] = ROUTE_MARKER_CP; p[14] = (uint8_t)(i + 1);
    }
    put32(data + 12, route_crc32(data + 96, *size - 96));
    return data;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *data)
{
    const uint16_t *pixel = (const uint16_t *)data;
    for (int y = area->y1; y <= area->y2; ++y)
        for (int x = area->x1; x <= area->x2; ++x) framebuffer[y * 240 + x] = *pixel++;
    lv_display_flush_ready(display);
}

static void save(const char *directory, const char *name)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s.ppm", directory, name);
    FILE *file = fopen(path, "wb"); assert(file);
    fprintf(file, "P6\n240 320\n255\n");
    for (size_t i = 0; i < 240 * 320; ++i) {
        const unsigned p = framebuffer[i];
        const uint8_t rgb[] = {(p >> 11) * 255 / 31, ((p >> 5) & 63) * 255 / 63, (p & 31) * 255 / 31};
        assert(fwrite(rgb, 1, 3, file) == 3);
    }
    fclose(file);
}

int main(int argc, char **argv)
{
    assert(argc <= 2);
    size_t size;
    uint8_t *data = fixture(&size);
    route_view_t route;
    assert(route_view_init(&route, data, (size_t)size, true));
    lv_init();
    lv_display_t *display = lv_display_create(240, 320); assert(display);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, flush_buffer, NULL, sizeof(flush_buffer), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    assert(route_ui_init() == ESP_OK);
    lv_mem_monitor_t before;
    lv_mem_monitor(&before);
    printf("Pool after UI init: %zu free\n", before.free_size); fflush(stdout);
    route_ui_model_t model = {.route = &route, .ble_connected = true, .gps_valid = true,
                              .battery_percent = 100, .route_m = 500};
    route_point_t point;
    assert(route_view_get_point(&route, 0, &point));
    model.current_lat_e7 = point.lat_e7; model.current_lon_e7 = point.lon_e7;
    route_ui_render(&model);
    lv_mem_monitor(&before);
    printf("Pool before draw: %zu free\n", before.free_size); fflush(stdout);
    lv_refr_now(display);
    assert(strcmp(lv_label_get_text(s_title), "崇州市绿道百...") == 0);
    assert(strcmp(lv_label_get_text(s_status), "BLE GPS") == 0);
    const lv_font_t *font = lv_obj_get_style_text_font(s_title, 0);
    const uint32_t required[] = {0x5d07, 0x5dde, 0x5e02, 0x7eff, 0x9053, 0x767e};
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        lv_font_glyph_dsc_t dsc;
        assert(lv_font_get_glyph_dsc(font, &dsc, required[i], 0) && !dsc.is_placeholder);
    }
    assert(lv_obj_get_x(s_title) + lv_obj_get_width(s_title) <= lv_obj_get_x(s_status));
    assert(lv_obj_get_x(s_status) + lv_obj_get_width(s_status) <= lv_obj_get_x(s_battery));
    for (unsigned i = 0; i < MAX_VISIBLE_MARKERS; ++i) {
        if (lv_obj_has_flag(s_marker_labels[i], LV_OBJ_FLAG_HIDDEN)) continue;
        const int x = lv_obj_get_x(s_marker_labels[i]), y = lv_obj_get_y(s_marker_labels[i]);
        assert(x >= 0 && y >= 0);
        assert(x + lv_obj_get_width(s_marker_labels[i]) <= MAP_W);
        assert(y + lv_obj_get_height(s_marker_labels[i]) <= MAP_H);
        for (unsigned j = 0; j < i; ++j) {
            if (lv_obj_has_flag(s_marker_labels[j], LV_OBJ_FLAG_HIDDEN)) continue;
            const int px = lv_obj_get_x(s_marker_labels[j]), py = lv_obj_get_y(s_marker_labels[j]);
            assert(x >= px + lv_obj_get_width(s_marker_labels[j]) || x + lv_obj_get_width(s_marker_labels[i]) <= px ||
                   y >= py + lv_obj_get_height(s_marker_labels[j]) || y + lv_obj_get_height(s_marker_labels[i]) <= py);
        }
    }
    if (argc == 2) save(argv[1], "route-map");
    route_ui_toggle_panel();
    route_ui_render(&model); lv_refr_now(display);
    assert(!lv_obj_has_flag(s_profile, LV_OBJ_FLAG_HIDDEN));
    if (argc == 2) save(argv[1], "route-profile");
    route.elevation_offset = 0;
    route_ui_render(&model); lv_refr_now(display);
    assert(lv_obj_has_flag(s_profile, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_cue_primary), "NO ELEVATION DATA") == 0);
    if (argc == 2) save(argv[1], "route-no-elevation");
    route_ui_toggle_panel();
    model.gps_valid = false; model.ble_connected = false;
    route_ui_render(&model); lv_refr_now(display);
    assert(strcmp(lv_label_get_text(s_status), "-- --") == 0);
    assert(lv_obj_has_flag(s_profile, LV_OBJ_FLAG_HIDDEN));
    lv_mem_monitor_t memory;
    lv_mem_monitor(&memory);
    assert(memory.free_size > 1024);
    printf("LVGL render, UTF-8, header layout, profile toggle: PASS (free pool: %zu)\n", memory.free_size);
    free(data);
    lv_deinit();
}
