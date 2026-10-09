#include "route_ui.h"

#include "lvgl.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MAP_X 8
#define MAP_Y 38
#define MAP_W 224
#define MAP_H 190
#define MAX_LINE_POINTS 128
#define MAX_VISIBLE_MARKERS 12

LV_FONT_DECLARE(lv_font_source_han_sans_sc_16_cjk);

static lv_obj_t *s_screen;
static lv_obj_t *s_title;
static lv_obj_t *s_status;
static lv_obj_t *s_battery;
static lv_obj_t *s_map;
static lv_obj_t *s_route_line;
static lv_obj_t *s_position;
static lv_obj_t *s_marker_objs[MAX_VISIBLE_MARKERS];
static lv_obj_t *s_marker_labels[MAX_VISIBLE_MARKERS];
static lv_obj_t *s_cue_card;
static lv_obj_t *s_cue_primary;
static lv_obj_t *s_cue_secondary;
static lv_point_precise_t s_line_points[MAX_LINE_POINTS];
static lv_font_t s_cjk_font;
static bool s_chinese_ok;
static bool s_follow = true;
static uint32_t s_center_route_m;
static int s_zoom_index = 2;

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
    lv_obj_set_style_clip_corner(obj, true, 0);
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
    lv_obj_set_width(s_title, 128);
    label_style(s_title, &lv_font_montserrat_14, 0xF2F7F8);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_title, "CTRA ROUTE");

    s_status = lv_label_create(s_screen);
    lv_obj_set_pos(s_status, 142, 10);
    lv_obj_set_width(s_status, 50);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_RIGHT, 0);
    label_style(s_status, &lv_font_montserrat_14, 0x74D9C3);
    lv_label_set_text(s_status, "BLE");

    s_battery = lv_label_create(s_screen);
    lv_obj_set_pos(s_battery, 194, 10);
    lv_obj_set_width(s_battery, 34);
    lv_obj_set_style_text_align(s_battery, LV_TEXT_ALIGN_RIGHT, 0);
    label_style(s_battery, &lv_font_montserrat_14, 0xF2F7F8);
    lv_label_set_text(s_battery, "--");

    s_map = panel(s_screen, MAP_X, MAP_Y, MAP_W, MAP_H, 0x0D1E25);

    s_route_line = lv_line_create(s_map);
    lv_obj_set_pos(s_route_line, 0, 0);
    lv_obj_set_size(s_route_line, MAP_W, MAP_H);
    lv_obj_set_style_line_color(s_route_line, lv_color_hex(0x5FE0C2), 0);
    lv_obj_set_style_line_width(s_route_line, 3, 0);
    lv_obj_set_style_line_rounded(s_route_line, true, 0);
    lv_line_set_points_mutable(s_route_line, s_line_points, 0);

    for (size_t i = 0; i < MAX_VISIBLE_MARKERS; ++i) {
        s_marker_objs[i] = lv_obj_create(s_map);
        lv_obj_remove_style_all(s_marker_objs[i]);
        lv_obj_set_size(s_marker_objs[i], 9, 9);
        lv_obj_set_style_radius(s_marker_objs[i], LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(s_marker_objs[i], lv_color_hex(0xF5C65A), 0);
        lv_obj_set_style_bg_opa(s_marker_objs[i], LV_OPA_COVER, 0);
        s_marker_labels[i] = lv_label_create(s_map);
        label_style(s_marker_labels[i], &lv_font_montserrat_14, 0xDCE6E8);
        lv_obj_add_flag(s_marker_objs[i], LV_OBJ_FLAG_HIDDEN);
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
    const float lat_rad = (float)center_lat * 1.0e-7f * 0.017453292519943295f;
    const float lon_scale = 0.011132f * cosf(lat_rad);
    const float dx = (float)(lon_e7 - center_lon) * lon_scale;
    const float dy = (float)(lat_e7 - center_lat) * 0.011132f;
    *x = MAP_W / 2 + (int)lroundf(dx / mpp);
    *y = MAP_H / 2 - (int)lroundf(dy / mpp);
}

static void render_map(const route_ui_model_t *model)
{
    const route_view_t *route = model->route;
    if (route == NULL || route->header.point_count < 2U) {
        lv_line_set_points_mutable(s_route_line, s_line_points, 0);
        lv_obj_add_flag(s_position, LV_OBJ_FLAG_HIDDEN);
        for (size_t i = 0; i < MAX_VISIBLE_MARKERS; ++i) {
            lv_obj_add_flag(s_marker_objs[i], LV_OBJ_FLAG_HIDDEN);
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

    const uint32_t approximate_span_m = (uint32_t)(MAP_H * mpp * 1.8f);
    uint32_t point_radius = approximate_span_m / 20U + 24U;
    if (point_radius > route->header.point_count) point_radius = route->header.point_count;
    const uint32_t start = center_index > point_radius ? center_index - point_radius : 0U;
    uint32_t end = center_index + point_radius;
    if (end >= route->header.point_count) end = route->header.point_count - 1U;

    uint32_t candidate_count = end - start + 1U;
    uint32_t step = candidate_count / MAX_LINE_POINTS + 1U;
    uint32_t count = 0;
    for (uint32_t i = start; i <= end && count < MAX_LINE_POINTS; i += step) {
        route_point_t p;
        if (!route_view_get_point(route, i, &p)) continue;
        int x;
        int y;
        project(p.lat_e7, p.lon_e7, center.lat_e7, center.lon_e7, mpp, &x, &y);
        if (x < -24 || x > MAP_W + 24 || y < -24 || y > MAP_H + 24) continue;
        s_line_points[count].x = x;
        s_line_points[count].y = y;
        ++count;
    }
    if (end == route->header.point_count - 1U && count < MAX_LINE_POINTS) {
        route_point_t p;
        (void)route_view_get_point(route, end, &p);
        int x;
        int y;
        project(p.lat_e7, p.lon_e7, center.lat_e7, center.lon_e7, mpp, &x, &y);
        if (x >= -24 && x <= MAP_W + 24 && y >= -24 && y <= MAP_H + 24) {
            s_line_points[count].x = x;
            s_line_points[count].y = y;
            ++count;
        }
    }
    lv_line_set_points_mutable(s_route_line, s_line_points, count);

    size_t marker_slot = 0;
    for (uint32_t i = 0; i < route->header.marker_count && marker_slot < MAX_VISIBLE_MARKERS; ++i) {
        route_marker_t marker;
        if (!route_view_get_marker(route, i, &marker)) continue;
        int x;
        int y;
        project(marker.lat_e7, marker.lon_e7, center.lat_e7, center.lon_e7, mpp, &x, &y);
        if (x < 6 || x > MAP_W - 6 || y < 6 || y > MAP_H - 6) continue;

        lv_obj_t *dot = s_marker_objs[marker_slot];
        lv_obj_t *label = s_marker_labels[marker_slot];
        lv_obj_set_pos(dot, x - 4, y - 4);
        uint32_t color = marker.type == ROUTE_MARKER_CP ? 0xF5C65A :
                         marker.type == ROUTE_MARKER_START ? 0x61D095 :
                         marker.type == ROUTE_MARKER_FINISH ? 0xF16D6D : 0x8FA3A8;
        lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);

        char text[12] = {0};
        if (marker.type == ROUTE_MARKER_CP) snprintf(text, sizeof(text), "CP%u", marker.id);
        else if (marker.type == ROUTE_MARKER_KM) snprintf(text, sizeof(text), "%uk", marker.id);
        else if (marker.type == ROUTE_MARKER_START) strcpy(text, "S");
        else if (marker.type == ROUTE_MARKER_FINISH) strcpy(text, "F");
        if (text[0] != '\0') {
            lv_label_set_text(label, text);
            lv_obj_set_pos(label, x + 5, y - 8);
            lv_obj_clear_flag(label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
        ++marker_slot;
    }
    for (; marker_slot < MAX_VISIBLE_MARKERS; ++marker_slot) {
        lv_obj_add_flag(s_marker_objs[marker_slot], LV_OBJ_FLAG_HIDDEN);
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

void route_ui_render(const route_ui_model_t *model)
{
    if (model == NULL || s_screen == NULL) return;

    if (model->route != NULL) {
        lv_label_set_text(s_title, model->route->header.name[0] != '\0'
            ? model->route->header.name : "CTRA ROUTE");
    } else {
        lv_label_set_text(s_title, "CTRA ROUTE");
    }

    char status[24];
    snprintf(status, sizeof(status), "%s%s",
             model->ble_connected ? "BLE" : "--",
             model->gps_valid ? " GPS" : "");
    lv_label_set_text(s_status, status);

    if (model->battery_percent >= 0) {
        char battery[12];
        snprintf(battery, sizeof(battery), "%d%%", model->battery_percent);
        lv_label_set_text(s_battery, battery);
    } else {
        lv_label_set_text(s_battery, "--");
    }

    render_map(model);

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
