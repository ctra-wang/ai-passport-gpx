"""Execute the actual startup helpers against display/BLE platform boundaries."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_deep_sleep_contract import function_body


ROOT = Path(__file__).resolve().parents[1]


class RouteStartupTests(unittest.TestCase):
    def test_ok_button_panel_recenter_and_voice_actions(self) -> None:
        body = function_body((ROOT / "main/route_app.c").read_text(), "process_button")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef enum { BSP_BTN_UP, BSP_BTN_DOWN, BSP_BTN_OK } bsp_btn_t;
typedef enum { BSP_BTN_CLICK, BSP_BTN_LONG, BSP_BTN_DOUBLE } bsp_btn_ev_t;
typedef struct { struct { struct { bsp_btn_t button; bsp_btn_ev_t action; } button; } data; } app_event_t;
static bool s_nav_ready;
static struct { uint32_t route_m; } s_nav;
static int toggles, replays, pans, zooms;
static uint32_t centered;
static void route_ui_toggle_panel(void) { ++toggles; }
static void route_ui_recenter(uint32_t distance) { centered = distance; }
static void replay_last_cue(void) { ++replays; }
static void route_ui_pan(int direction) { pans += direction; }
static void route_ui_zoom(int direction) { zooms += direction; }
static void process_button(const app_event_t *event) {
''' + body + r'''
}
int main(void) {
    app_event_t event = {.data.button = {BSP_BTN_OK, BSP_BTN_CLICK}};
    process_button(&event); assert(toggles == 1 && !replays);
    event.data.button.action = BSP_BTN_DOUBLE;
    process_button(&event); assert(replays == 1 && toggles == 1);
    event.data.button.action = BSP_BTN_LONG;
    s_nav_ready = true; s_nav.route_m = 1234;
    process_button(&event); assert(centered == 1234 && toggles == 1);
    s_nav_ready = false; process_button(&event); assert(centered == 0);
    event.data.button.button = BSP_BTN_UP; process_button(&event); assert(zooms == 1);
    event.data.button.action = BSP_BTN_CLICK; process_button(&event); assert(pans == 1);
}
''')

    def run_c(self, source: str) -> None:
        # Keep generated files: the local workflow prohibits bulk deletion.
        directory = Path(tempfile.mkdtemp(prefix="ai-passport-startup-tests-"))
        binary = directory / "test"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
             "-x", "c", "-", "-o", str(binary)],
            input=source, text=True, check=True,
        )
        subprocess.run([str(binary)], check=True, timeout=5)

    def test_advertising_packets_fit_and_failures_stop_startup(self) -> None:
        source = (ROOT / "main/route_ble.c").read_text()
        name = next(line for line in source.splitlines()
                    if line.startswith("#define ROUTE_BLE_DEVICE_NAME "))
        body = function_body(source, "start_advertising")
        self.run_c(r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define BLE_HS_ADV_F_DISC_GEN 2
#define BLE_HS_ADV_F_BREDR_UNSUP 4
#define BLE_GAP_CONN_MODE_UND 2
#define BLE_GAP_DISC_MODE_GEN 2
#define BLE_HS_FOREVER INT32_MAX
struct ble_hs_adv_fields {
    uint8_t flags, name_len, name_is_complete;
    const uint8_t *name;
    const void *uuids128;
    uint8_t num_uuids128, uuids128_is_complete;
};
struct ble_gap_adv_params { int conn_mode, disc_mode; };
struct ble_gap_event { int type; };
static const uint8_t s_service_uuid[16] = {0};
static uint8_t s_addr_type = 1;
static int phase, fail_phase;
static unsigned packet_size(const struct ble_hs_adv_fields *f) {
    return (f->flags ? 3 : 0) + (f->name_len ? 2 + f->name_len : 0) +
           (f->num_uuids128 ? 2 + 16 * f->num_uuids128 : 0);
}
static int ble_gap_adv_set_fields(const struct ble_hs_adv_fields *f) {
    assert(++phase == 1);
    assert(packet_size(f) <= 31);
    assert(f->flags == 6 && f->uuids128 == &s_service_uuid);
    assert(f->num_uuids128 == 1 && f->uuids128_is_complete);
    assert(!f->name && !f->name_len);
    return fail_phase == phase ? 101 : 0;
}
int ble_gap_adv_rsp_set_fields(const struct ble_hs_adv_fields *f) {
    assert(++phase == 2);
    assert(packet_size(f) <= 31 && f->name_is_complete);
    assert(f->name_len == 13 && memcmp(f->name, "CTRA-Passport", 13) == 0);
    return fail_phase == phase ? 102 : 0;
}
static int route_gap_event(struct ble_gap_event *event, void *arg) {
    (void)event; (void)arg; return 0;
}
static int ble_gap_adv_start(uint8_t address, const void *peer, int duration,
                            const struct ble_gap_adv_params *params,
                            int (*callback)(struct ble_gap_event *, void *), void *arg) {
    assert(++phase == 3);
    assert(address == s_addr_type && !peer && duration == BLE_HS_FOREVER);
    assert(params->conn_mode == BLE_GAP_CONN_MODE_UND);
    assert(params->disc_mode == BLE_GAP_DISC_MODE_GEN);
    assert(callback == route_gap_event && !arg);
    return fail_phase == phase ? 103 : 0;
}
''' + name + "\nstatic int start_advertising(void) {" + body + r'''
}
int main(void) {
    for (fail_phase = 0; fail_phase <= 3; ++fail_phase) {
        phase = 0;
        assert(start_advertising() == (fail_phase ? 100 + fail_phase : 0));
        assert(phase == (fail_phase ? fail_phase : 3));
    }
    fail_phase = 0;
    phase = 0;
    assert(start_advertising() == 0 && phase == 3);
}
''')

    def test_panels_keep_rounded_background_without_child_clip_layers(self) -> None:
        body = function_body((ROOT / "main/route_ui.c").read_text(), "panel")
        self.run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#define LV_OPA_COVER 255
#define LV_OBJ_FLAG_SCROLLABLE 1
typedef struct { int width, height, radius; bool child_clip; } lv_obj_t;
static lv_obj_t object;
static lv_obj_t *lv_obj_create(lv_obj_t *parent) { (void)parent; return &object; }
static void lv_obj_remove_style_all(lv_obj_t *obj) { *obj = (lv_obj_t){0}; }
static void lv_obj_set_pos(lv_obj_t *obj, int x, int y) { (void)obj; (void)x; (void)y; }
static void lv_obj_set_size(lv_obj_t *obj, int w, int h) { obj->width = w; obj->height = h; }
static uint32_t lv_color_hex(uint32_t color) { return color; }
static void lv_obj_set_style_bg_color(lv_obj_t *obj, uint32_t c, int s) { (void)obj; (void)c; (void)s; }
static void lv_obj_set_style_bg_opa(lv_obj_t *obj, int opa, int s) { (void)obj; (void)s; assert(opa == 255); }
static void lv_obj_set_style_radius(lv_obj_t *obj, int r, int s) { (void)s; obj->radius = r; }
void lv_obj_set_style_clip_corner(lv_obj_t *obj, bool clip, int s) { (void)s; obj->child_clip = clip; }
static void lv_obj_clear_flag(lv_obj_t *obj, int flag) { (void)obj; assert(flag == LV_OBJ_FLAG_SCROLLABLE); }
static lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color) {
''' + body + r'''
}
int main(void) {
    const int heights[] = {190, 74};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *obj = panel(&object, 8, 38, 224, heights[i], 0x0d1e25);
        assert(obj->width == 224 && obj->height == heights[i]);
        assert(obj->radius == 14 && !obj->child_clip);
    }
}
''')


if __name__ == "__main__":
    unittest.main()
