#include "route_ui.h"
#include "route_ui_geometry.h"

#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAP_X 8
#define MAP_Y 38
#define MAP_W 224
#define MAP_H 190
#define MAX_VISIBLE_MARKERS 12
#define PROFILE_W 200
#define PROFILE_H 40

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);
LV_FONT_DECLARE(noto_sans_sc_14);

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_battery;
static lv_obj_t *s_map;
static lv_obj_t *s_route_line;
static lv_obj_t *s_position;
static lv_obj_t *s_marker_labels[MAX_VISIBLE_MARKERS];
static lv_obj_t *s_cue_card;
static lv_obj_t *s_cue_primary;
static lv_obj_t *s_cue_secondary;
static lv_obj_t *s_profile;
static lv_obj_t *s_profile_position;
LV_DRAW_BUF_DEFINE_STATIC(s_map_buffer, MAP_W, MAP_H, LV_COLOR_FORMAT_I2);
LV_DRAW_BUF_DEFINE_STATIC(s_profile_buffer, PROFILE_W, PROFILE_H, LV_COLOR_FORMAT_I2);
static bool s_show_profile;
static lv_font_t s_cjk_font;
static bool s_chinese_ok;
static bool s_follow = true;
static uint32_t s_center_route_m;
static int s_zoom_index = 2;
static float s_longitude_scale;

static const float s_zoom_mpp[] = {1.5f, 3.0f, 6.0f, 12.0f, 24.0f};

static bool font_has(uint32_t cp)
{
    lv_font_glyph_dsc_t dsc = {0};
    return lv_font_get_glyph_dsc(&lv_font_source_han_sans_sc_16_cjk, &dsc, cp, 0) &&
           !dsc.is_placeholder;
}

static bool verify_required_chinese(void)
{
    /*
     * Keep the fixed Chinese vocabulary inside LVGL 9.5.0's built-in
     * SourceHanSansSC subset. Runtime verification still guards against a
     * dependency/font change before any Chinese label is shown.
     */
    static const uint32_t required[] = {
        0x8DEF, 0x7A0B, /* 路程 */
        0x7B49, 0x5F85, 0x5B9A, 0x4F4D, /* 等待定位 */
        0x5916, /* 外 */
        0x5411, 0x5DE6, 0x53F3, /* 向左右 */
        0x5206, /* 分 */
        0x7AD9, 0x70B9, /* 站点 */
        0x6C34, /* 水 */
        0x4E0A, 0x4E0B, 0x884C, /* 上下行 */
        0x6CE8, 0x610F, /* 注意 */
        0x5B8C, 0x6210, /* 完成 */
        0x63D0, 0x793A, /* 提示 */
        0x524D, 0x65B9, 0x7C73, /* 前方米 */
        0x540C, 0x6B65, 0x4E2D, /* 同步中 */
    };
    for (size_t i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        if (!font_has(required[i])) return false;
    }
    return true;
}

static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_style_all(obj);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(obj, 14, 0);
    /* Child clipping allocates ARGB layers that can exhaust the 24 KB LVGL pool. */
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

static void label_style(lv_obj_t *label, const lv_font_t *font, uint32_t color)
{
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
}

static const char *cue_name(uint8_t type)
{
    if (!s_chinese_ok) {
        switch (type) {
        case ROUTE_CUE_LEFT: return "LEFT";
        case ROUTE_CUE_RIGHT: return "RIGHT";
        case ROUTE_CUE_FORK: return "FORK";
        case ROUTE_CUE_CP: return "CHECKPOINT";
        case ROUTE_CUE_CLIMB: return "CLIMB";
        case ROUTE_CUE_DESCENT: return "DESCENT";
        case ROUTE_CUE_DANGER: return "DANGER";
        case ROUTE_CUE_SUPPLY: return "SUPPLY";
        case ROUTE_CUE_FINISH: return "FINISH";
        default: return "ROADBOOK";
        }
    }
    switch (type) {
    case ROUTE_CUE_LEFT: return "向左";
    case ROUTE_CUE_RIGHT: return "向右";
    case ROUTE_CUE_FORK: return "分路";
    case ROUTE_CUE_CP: return "站点";
    case ROUTE_CUE_CLIMB: return "上行";
    case ROUTE_CUE_DESCENT: return "下行";
    case ROUTE_CUE_DANGER: return "注意";
    case ROUTE_CUE_SUPPLY: return "水站";
    case ROUTE_CUE_FINISH: return "完成";
    default: return "提示";
    }
}

esp_err_t route_ui_init(void)
{
    s_cjk_font = lv_font_source_han_sans_sc_16_cjk;
    s_cjk_font.fallback = &lv_font_montserrat_14;
    s_chinese_ok = verify_required_chinese();

    s_screen = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_screen);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x071117), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);

    s_title = lv_label_create(s_screen);
    lv_obj_set_pos(s_title, 12, 10);
    lv_obj_set_size(s_title, 96, 20);
    label_style(s_title, &noto_sans_sc_14, 0xF2F7F8);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);
    lv_label_set_text(s_title, "CTRA ROUTE");

    s_status = lv_label_create(s_screen);
    lv_obj_set_pos(s_status, 110, 12);
    lv_obj_set_size(s_status, 74, 18);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    label_style(s_status, &lv_font_montserrat_12, 0x74D9C3);
    lv_label_set_text(s_status, "BLE");

    s_battery = lv_label_create(s_screen);
    lv_obj_set_pos(s_battery, 188, 12);
    lv_obj_set_size(s_battery, 40, 18);
    lv_label_set_long_mode(s_battery, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    label_style(s_battery, &lv_font_montserrat_12, 0xF2F7F8);
    lv_label_set_text(s_battery, "--");

    s_map = panel(s_screen, MAP_X, MAP_Y, MAP_W, MAP_H, 0x0D1E25);

    s_route_line = lv_canvas_create(s_map);
    lv_obj_set_pos(s_route_line, 0, 0);
    LV_DRAW_BUF_INIT_STATIC(s_map_buffer);
    lv_canvas_set_draw_buf(s_route_line, &s_map_buffer);
    lv_canvas_set_palette(s_route_line, 0, lv_color32_make(13, 30, 37, 255));
    lv_canvas_set_palette(s_route_line, 1, lv_color32_make(76, 112, 132, 255));
    lv_canvas_set_palette(s_route_line, 2, lv_color32_make(95, 224, 194, 255));
    lv_canvas_set_palette(s_route_line, 3, lv_color32_make(245, 198, 90, 255));

    for (size_t i = 0; i < MAX_VISIBLE_MARKERS; ++i) {
        s_marker_labels[i] = lv_label_create(s_map);
        label_style(s_marker_labels[i], &lv_font_montserrat_14, 0xDCE6E8);
        lv_obj_add_flag(s_marker_labels[i], LV_OBJ_FLAG_HIDDEN);
    }

    s_position = lv_obj_create(s_map);
    lv_obj_remove_style_all(s_position);
    lv_obj_set_size(s_position, 12, 12);
    lv_obj_set_style_radius(s_position, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_position, lv_color_hex(0xFF7448), 0);
    lv_obj_set_style_bg_opa(s_position, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_position, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(s_position, 2, 0);
    lv_obj_add_flag(s_position, LV_OBJ_FLAG_HIDDEN);

    s_cue_card = panel(s_screen, 8, 236, 224, 74, 0xF4F1E8);
    s_cue_primary = lv_label_create(s_cue_card);
    lv_obj_set_pos(s_cue_primary, 12, 10);
    lv_obj_set_width(s_cue_primary, 200);
    label_style(s_cue_primary, s_chinese_ok ? &s_cjk_font : &lv_font_montserrat_14, 0x14262C);
    lv_label_set_text(s_cue_primary, s_chinese_ok ? "等待定位" : "WAITING FOR GPS");

    s_cue_secondary = lv_label_create(s_cue_card);
    lv_obj_set_pos(s_cue_secondary, 12, 40);
    lv_obj_set_width(s_cue_secondary, 200);
    label_style(s_cue_secondary, &lv_font_montserrat_14, 0x52666C);
    lv_label_set_text(s_cue_secondary, "UP/DOWN pan  HOLD zoom");

    s_profile = lv_canvas_create(s_cue_card);
    lv_obj_set_pos(s_profile, 12, 25);
    LV_DRAW_BUF_INIT_STATIC(s_profile_buffer);
    lv_canvas_set_draw_buf(s_profile, &s_profile_buffer);
    lv_canvas_set_palette(s_profile, 0, lv_color32_make(244, 241, 232, 255));
    lv_canvas_set_palette(s_profile, 1, lv_color32_make(82, 102, 108, 255));
    s_profile_position = panel(s_profile, 0, 0, 2, PROFILE_H, 0xFF7448);
    lv_obj_add_flag(s_profile, LV_OBJ_FLAG_HIDDEN);

    lv_screen_load(s_screen);
    return ESP_OK;
}

static uint32_t center_point_index(const route_view_t *route, uint32_t route_m)
{
    uint32_t lo = 0;
    uint32_t hi = route->header.point_count - 1U;
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2U;
        route_point_t p;
        if (!route_view_get_point(route, mid, &p)) break;
        if (p.route_m < route_m) lo = mid + 1U;
        else hi = mid;
    }
    return lo;
}

static void project(int32_t lat_e7, int32_t lon_e7, int32_t center_lat,
                    int32_t center_lon, float mpp, int *x, int *y)
{
    const float dx = (float)((double)lon_e7 - center_lon) * s_longitude_scale;
    const float dy = (float)((double)lat_e7 - center_lat) * 0.011132f;
    *x = MAP_W / 2 + (int)lroundf(dx / mpp);
    *y = MAP_H / 2 - (int)lroundf(dy / mpp);
}

static void render_map(const route_ui_model_t *model)
{
    const route_view_t *route = model->route;
    uint8_t *pixels = lv_draw_buf_goto_xy(&s_map_buffer, 0, 0);
    const size_t stride = s_map_buffer.header.stride;
    memset(pixels, 0, stride * MAP_H);
    if (route == NULL || route->header.point_count < 2U) {
        lv_obj_invalidate(s_route_line);
        lv_obj_add_flag(s_position, LV_OBJ_FLAG_HIDDEN);
        for (size_t i = 0; i < MAX_VISIBLE_MARKERS; ++i) {
            lv_obj_add_flag(s_marker_labels[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }

    if (s_follow && model->gps_valid) s_center_route_m = model->route_m;
    if (s_center_route_m > route->header.route_distance_m) {
        s_center_route_m = route->header.route_distance_m;
    }

    const uint32_t center_index = center_point_index(route, s_center_route_m);
    route_point_t center;
    (void)route_view_get_point(route, center_index, &center);
    const float mpp = s_zoom_mpp[s_zoom_index];
    s_longitude_scale = 0.011132f * cosf((float)center.lat_e7 * 1.0e-7f * 0.017453292519943295f);

    /* Fixed-size pixels retain all visible segments, including distant loopbacks. */
    route_point_t previous;
    (void)route_view_get_point(route, 0, &previous);
    int x0, y0;
    project(previous.lat_e7, previous.lon_e7, center.lat_e7, center.lon_e7, mpp, &x0, &y0);
    for (uint32_t i = 1; i < route->header.point_count; ++i) {
        route_point_t p;
        (void)route_view_get_point(route, i, &p);
        int x1, y1;
        project(p.lat_e7, p.lon_e7, center.lat_e7, center.lon_e7, mpp, &x1, &y1);
        const bool passed = model->gps_valid && p.route_m <= model->route_m;
        route_ui_plot_line(pixels, MAP_W, MAP_H, stride, x0, y0, x1, y1, passed ? 1 : 2);
        x0 = x1; y0 = y1;
    }
    lv_obj_invalidate(s_route_line);

    size_t marker_slot = 0;
    for (uint32_t i = 0; i < route->header.marker_count; ++i) {
        route_marker_t marker;
        if (!route_view_get_marker(route, i, &marker)) continue;
        int x;
        int y;
        project(marker.lat_e7, marker.lon_e7, center.lat_e7, center.lon_e7, mpp, &x, &y);
        if (x < 6 || x > MAP_W - 6 || y < 6 || y > MAP_H - 6) continue;

        route_ui_plot_line(pixels, MAP_W, MAP_H, stride, x, y, x, y, 3);
        if (marker_slot >= MAX_VISIBLE_MARKERS) continue;
        lv_obj_t *label = s_marker_labels[marker_slot];
        uint32_t color = marker.type == ROUTE_MARKER_CP ? 0xF5C65A :
                         marker.type == ROUTE_MARKER_START ? 0x61D095 :
                         marker.type == ROUTE_MARKER_FINISH ? 0xF16D6D : 0x8FA3A8;
        lv_obj_set_style_text_color(label, lv_color_hex(color), 0);

        char text[12] = {0};
        if (marker.type == ROUTE_MARKER_CP) snprintf(text, sizeof(text), "CP%u", marker.id);
        else if (marker.type == ROUTE_MARKER_KM) snprintf(text, sizeof(text), "%uk", marker.id);
        else if (marker.type == ROUTE_MARKER_START) strcpy(text, "S");
        else if (marker.type == ROUTE_MARKER_FINISH) strcpy(text, "F");
        if (text[0] == '\0') continue;
        lv_label_set_text(label, text);
        lv_obj_update_layout(label);
        const int w = lv_obj_get_width(label), h = lv_obj_get_height(label);
        int lx = x + 5, ly = y - 8;
        if (lx + w > MAP_W - 3) lx = MAP_W - 3 - w;
        if (ly < 3) ly = 3;
        if (ly + h > MAP_H - 3) ly = MAP_H - 3 - h;
        bool collision = false;
        for (size_t j = 0; j < marker_slot; ++j) {
            const int px = lv_obj_get_x(s_marker_labels[j]), py = lv_obj_get_y(s_marker_labels[j]);
            if (lx < px + lv_obj_get_width(s_marker_labels[j]) + 2 && lx + w + 2 > px &&
                ly < py + lv_obj_get_height(s_marker_labels[j]) + 2 && ly + h + 2 > py) collision = true;
        }
        if (collision) continue;
        lv_obj_set_pos(label, lx, ly);
        lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        ++marker_slot;
    }
    for (; marker_slot < MAX_VISIBLE_MARKERS; ++marker_slot) {
        lv_obj_add_flag(s_marker_labels[marker_slot], LV_OBJ_FLAG_HIDDEN);
    }

    if (model->gps_valid) {
        int x;
        int y;
        project(model->current_lat_e7, model->current_lon_e7,
                center.lat_e7, center.lon_e7, mpp, &x, &y);
        if (x >= 0 && x < MAP_W && y >= 0 && y < MAP_H) {
            lv_obj_set_pos(s_position, x - 6, y - 6);
            lv_obj_clear_flag(s_position, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_position, LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        lv_obj_add_flag(s_position, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_profile(const route_ui_model_t *model)
{
    const route_view_t *route = model->route;
    int32_t minimum = INT32_MAX, maximum = INT32_MIN;
    unsigned valid = 0;
    for (uint32_t i = 0; i < route->header.point_count; ++i) {
        int32_t dm;
        if (!route_view_get_elevation(route, i, &dm)) continue;
        if (dm < minimum) minimum = dm;
        if (dm > maximum) maximum = dm;
        ++valid;
    }
    if (valid < 2) {
        lv_label_set_text(s_cue_primary, "NO ELEVATION DATA");
        lv_label_set_text(s_cue_secondary, "Import GPX with elevation again");
        return;
    }
    uint8_t *pixels = lv_draw_buf_goto_xy(&s_profile_buffer, 0, 0);
    const size_t stride = s_profile_buffer.header.stride;
    memset(pixels, 0, stride * PROFILE_H);
    const double distance = route->header.route_distance_m ? route->header.route_distance_m : 1;
    const double span = maximum > minimum ? maximum - minimum : 10;
    int previous_x = 0, previous_y = 0;
    bool have_previous = false;
    for (uint32_t i = 0; i < route->header.point_count; ++i) {
        route_point_t point;
        int32_t dm;
        if (!route_view_get_elevation(route, i, &dm)) { have_previous = false; continue; }
        (void)route_view_get_point(route, i, &point);
        int x = (int)lround(point.route_m / distance * (PROFILE_W - 1));
        int y = maximum == minimum ? PROFILE_H / 2 :
                PROFILE_H - 3 - (int)lround((dm - minimum) / span * (PROFILE_H - 6));
        if (have_previous) route_ui_plot_line(pixels, PROFILE_W, PROFILE_H, stride, previous_x, previous_y, x, y, 1);
        previous_x = x; previous_y = y; have_previous = true;
    }
    char text[64];
    snprintf(text, sizeof(text), "%.0f..%.0fm  %.1fkm", minimum / 10.0f, maximum / 10.0f,
             route->header.route_distance_m / 1000.0f);
    lv_label_set_text(s_cue_primary, text);
    lv_obj_clear_flag(s_profile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cue_secondary, LV_OBJ_FLAG_HIDDEN);
    if (model->gps_valid && model->route_m <= route->header.route_distance_m) {
        lv_obj_set_x(s_profile_position, (int)lround(model->route_m / distance * (PROFILE_W - 2)));
        lv_obj_clear_flag(s_profile_position, LV_OBJ_FLAG_HIDDEN);
    } else lv_obj_add_flag(s_profile_position, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_profile);
}

void route_ui_render(const route_ui_model_t *model)
{
    if (model == NULL || s_screen == NULL) return;

    if (model->route != NULL) {
        char title[28];
        route_ui_title(model->route->header.name[0] != '\0' ? model->route->header.name : "CTRA ROUTE", title);
        lv_label_set_text(s_title, title);
    } else {
        lv_label_set_text(s_title, "CTRA ROUTE");
    }

    char status[24];
    snprintf(status, sizeof(status), "%s %s",
             model->ble_connected ? "BLE" : "--",
             model->gps_valid ? "GPS" : "--");
    lv_label_set_text(s_status, status);

    if (model->battery_percent >= 0) {
        char battery[12];
        snprintf(battery, sizeof(battery), "%d%%", model->battery_percent);
        lv_label_set_text(s_battery, battery);
    } else {
        lv_label_set_text(s_battery, "--");
    }

    render_map(model);
    label_style(s_cue_primary, s_chinese_ok ? &s_cjk_font : &lv_font_montserrat_14, 0x14262C);
    lv_obj_add_flag(s_profile, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_cue_secondary, LV_OBJ_FLAG_HIDDEN);

    if (model->upload_active) {
        char line[48];
        snprintf(line, sizeof(line), "%s %u%%",
                 s_chinese_ok ? "同步中" : "SYNC", model->upload_percent);
        lv_label_set_text(s_cue_primary, line);
        lv_label_set_text(s_cue_secondary, "Keep CTRA miniapp connected");
        return;
    }

    if (model->route == NULL) {
        lv_label_set_text(s_cue_primary, s_chinese_ok ? "等待路程" : "WAITING FOR ROUTE");
        lv_label_set_text(s_cue_secondary, "Connect CTRA miniapp via BLE");
        return;
    }

    if (s_show_profile) {
        label_style(s_cue_primary, &lv_font_montserrat_12, 0x14262C);
        render_profile(model);
        return;
    }

    if (!model->gps_valid) {
        lv_label_set_text(s_cue_primary, s_chinese_ok ? "等待定位" : "WAITING FOR GPS");
        lv_label_set_text(s_cue_secondary, model->ble_connected ? "CTRA GPS --" : "BLE disconnected");
        return;
    }

    if (model->offroute) {
        lv_label_set_text(s_cue_primary, s_chinese_ok ? "路外" : "OFF ROUTE");
        char detail[48];
        snprintf(detail, sizeof(detail), "%.0fm from route", model->cross_track_m);
        lv_label_set_text(s_cue_secondary, detail);
        return;
    }

    if (model->have_next_cue) {
        const int32_t remain = (int32_t)model->next_cue.route_m - (int32_t)model->route_m;
        char primary[48];
        if (s_chinese_ok) {
            snprintf(primary, sizeof(primary), "前方 %ld 米 %s",
                     (long)(remain > 0 ? remain : 0), cue_name(model->next_cue.type));
        } else {
            snprintf(primary, sizeof(primary), "%ldm  %s",
                     (long)(remain > 0 ? remain : 0), cue_name(model->next_cue.type));
        }
        lv_label_set_text(s_cue_primary, primary);
    } else {
        lv_label_set_text(s_cue_primary, s_chinese_ok ? "路上" : "ON ROUTE");
    }

    char secondary[64];
    snprintf(secondary, sizeof(secondary), "%.1f / %.1f km  x%.1f%s",
             model->route_m / 1000.0f,
             model->route->header.route_distance_m / 1000.0f,
             s_zoom_mpp[2] / s_zoom_mpp[s_zoom_index],
             s_follow ? "  FOLLOW" : "  BROWSE");
    lv_label_set_text(s_cue_secondary, secondary);
}

void route_ui_pan(int direction)
{
    if (s_screen == NULL || direction == 0) return;
    s_follow = false;
    const uint32_t delta = (uint32_t)(MAP_H * s_zoom_mpp[s_zoom_index] * 0.35f);
    if (direction > 0) {
        s_center_route_m += delta;
    } else {
        s_center_route_m = s_center_route_m > delta ? s_center_route_m - delta : 0U;
    }
}

void route_ui_zoom(int direction)
{
    if (direction > 0 && s_zoom_index > 0) --s_zoom_index;
    if (direction < 0 && s_zoom_index < (int)(sizeof(s_zoom_mpp) / sizeof(s_zoom_mpp[0])) - 1) {
        ++s_zoom_index;
    }
}

void route_ui_recenter(uint32_t route_m)
{
    s_center_route_m = route_m;
    s_follow = true;
}

bool route_ui_following(void)
{
    return s_follow;
}

void route_ui_toggle_panel(void)
{
    s_show_profile = !s_show_profile;
}
