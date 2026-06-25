/*
 * Tab5-Meshtastic v2 — app shell (UI layer), M0.
 *
 * Static chrome: left rail (NODES / CHAT / RADIO), status bar, three empty
 * content panels with milestone placeholders. No live data and no BLE — the
 * backend layers land in M1+. All object creation runs under the LVGL lock.
 */

#include "ui_shell.h"
#include "theme.h"

#include "lvgl.h"
#include "lvgl_port.h"

#include <esp_log.h>

namespace {

const char* TAG = "ui_shell";

constexpr int NUM_TABS = 3;
const char* kNavText[NUM_TABS] = {"NODES", "CHAT", "RADIO"};
const char* kNavIcon[NUM_TABS] = {LV_SYMBOL_LIST, LV_SYMBOL_KEYBOARD, LV_SYMBOL_WIFI};

struct ShellState {
    lv_obj_t* nav[NUM_TABS]   = {};
    lv_obj_t* panel[NUM_TABS] = {};
    int active                = 0;
};
ShellState S;

/* ---- small style helpers ---- */

lv_obj_t* box(lv_obj_t* parent, int w, int h)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

void bg(lv_obj_t* o, uint32_t c)
{
    lv_obj_set_style_bg_color(o, lv_color_hex(c), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
}

void radius(lv_obj_t* o, int r) { lv_obj_set_style_radius(o, r, 0); }

lv_obj_t* label(lv_obj_t* parent, const char* t, const lv_font_t* f, uint32_t c)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
    return l;
}

void flex_row(lv_obj_t* o)
{
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
}

void flex_col(lv_obj_t* o)
{
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
}

void hairline_side(lv_obj_t* o, lv_border_side_t side)
{
    lv_obj_set_style_border_width(o, 1, 0);
    lv_obj_set_style_border_color(o, lv_color_hex(C_SURF), 0);
    lv_obj_set_style_border_side(o, side, 0);
}

/* ---- tab switching ---- */

void set_tab(int i)
{
    if (i < 0 || i >= NUM_TABS) return;
    S.active = i;
    for (int t = 0; t < NUM_TABS; t++) {
        bool on = (t == i);
        if (S.panel[t]) {
            if (on) lv_obj_clear_flag(S.panel[t], LV_OBJ_FLAG_HIDDEN);
            else    lv_obj_add_flag(S.panel[t], LV_OBJ_FLAG_HIDDEN);
        }
        if (S.nav[t]) {
            lv_obj_set_style_bg_color(S.nav[t], lv_color_hex(on ? C_SURF : C_CHROME), 0);
            lv_obj_set_style_bg_opa(S.nav[t], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        }
    }
}

void nav_cb(lv_event_t* e) { set_tab((int)(intptr_t)lv_event_get_user_data(e)); }

/* A content panel: header title + centered milestone placeholder. */
lv_obj_t* make_panel(lv_obj_t* parent, const char* title, const char* hint)
{
    lv_obj_t* panel = box(parent, lv_pct(100), lv_pct(100));
    flex_col(panel);

    lv_obj_t* hdr = box(panel, lv_pct(100), 54);
    flex_row(hdr);
    lv_obj_set_style_pad_hor(hdr, 20, 0);
    hairline_side(hdr, LV_BORDER_SIDE_BOTTOM);
    label(hdr, title, FONT_ROW, C_HI);

    lv_obj_t* body = box(panel, lv_pct(100), 0);
    lv_obj_set_flex_grow(body, 1);
    lv_obj_t* h = label(body, hint, FONT_BODY, C_DIM);
    lv_obj_center(h);
    return panel;
}

void build_shell(void)
{
    lv_obj_t* scr = lv_screen_active();
    bg(scr, C_BG);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* root = box(scr, 1280, 720);
    flex_row(root);

    /* ---- side rail ---- */
    lv_obj_t* rail = box(root, M_RAIL_W, 720);
    bg(rail, C_CHROME);
    flex_col(rail);
    lv_obj_set_flex_align(rail, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(rail, 14, 0);
    lv_obj_set_style_pad_row(rail, 8, 0);
    hairline_side(rail, LV_BORDER_SIDE_RIGHT);

    lv_obj_t* logo = box(rail, 44, 44);
    bg(logo, C_GREEN);
    radius(logo, M_RAD_M);
    lv_obj_center(label(logo, LV_SYMBOL_WIFI, FONT_BODY, C_INK));

    for (int i = 0; i < NUM_TABS; i++) {
        lv_obj_t* b = box(rail, M_RAIL_BTN, M_RAIL_BTN);
        radius(b, M_RAD_L);
        flex_col(b);
        lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(b, 5, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        label(b, kNavIcon[i], FONT_BODY, C_MID);
        label(b, kNavText[i], FONT_META, C_MID);
        S.nav[i] = b;
    }

    /* ---- main column ---- */
    lv_obj_t* col = box(root, 0, 720);
    lv_obj_set_flex_grow(col, 1);
    flex_col(col);

    /* status bar */
    lv_obj_t* sb = box(col, lv_pct(100), M_STATUS_H);
    bg(sb, C_CHROME);
    flex_row(sb);
    lv_obj_set_style_pad_hor(sb, 18, 0);
    lv_obj_set_style_pad_column(sb, 16, 0);
    hairline_side(sb, LV_BORDER_SIDE_BOTTOM);

    lv_obj_t* mb = box(sb, 36, 36);
    bg(mb, C_SURF);
    radius(mb, M_RAD_S);
    lv_obj_center(label(mb, "--", FONT_META, C_MID));

    lv_obj_t* me = box(sb, 0, 40);
    lv_obj_set_width(me, LV_SIZE_CONTENT);
    flex_col(me);
    label(me, "No device", FONT_META, C_HI);
    label(me, "not connected", FONT_META, C_DIM);

    lv_obj_t* spacer = box(sb, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);

    /* link-state chip — amber "NO DEVICE" until M1/M5 wire the real state */
    lv_obj_t* chip = box(sb, 0, 32);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    flex_row(chip);
    lv_obj_set_style_pad_hor(chip, 13, 0);
    lv_obj_set_style_pad_column(chip, 9, 0);
    radius(chip, M_RAD_M);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(C_SURF), 0);
    label(chip, "NO DEVICE", FONT_META, C_AMBER);

    lv_obj_t* cnt = box(sb, 0, 32);
    lv_obj_set_width(cnt, LV_SIZE_CONTENT);
    flex_row(cnt);
    lv_obj_set_style_pad_column(cnt, 7, 0);
    label(cnt, LV_SYMBOL_WIFI, FONT_META, C_MID);
    label(cnt, "0", FONT_META, C_MID);

    label(sb, "--:--", FONT_BODY, C_HI);

    /* content area + panels */
    lv_obj_t* content = box(col, lv_pct(100), 0);
    lv_obj_set_flex_grow(content, 1);
    bg(content, C_BG);

    S.panel[0] = make_panel(content, "Mesh nodes", "Node sync lands in M1-M2");
    S.panel[1] = make_panel(content, "Primary  -  broadcast", "Chat lands in M4");
    S.panel[2] = make_panel(content, "Radio", "Onboarding / device picker lands in M5");

    set_tab(0);
    ESP_LOGI(TAG, "shell built");
}

}  // namespace

void ui_start(void)
{
    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "could not lock LVGL to build the shell");
        return;
    }
    build_shell();
    lvgl_port_unlock();
}
