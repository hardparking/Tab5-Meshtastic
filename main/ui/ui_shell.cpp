/*
 * Tab5-Meshtastic v2 — app shell (UI layer).
 *
 * Static chrome (rail / status bar / content panels) plus a refresh timer that
 * is the ONLY BLE→UI channel: it runs on the LVGL task, reads an immutable
 * app_state snapshot, and updates the status chip / my-node badge / node count
 * / diagnostics strip. The backend never touches LVGL (PRD §5).
 */

#include "ui_shell.h"
#include "theme.h"
#include "app_state.h"

#include "lvgl.h"
#include "lvgl_port.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* On-screen diagnostics overlay (FR-5.1). Dev builds show it; a "release" build
 * sets this to 0. Serial can't be attached without resetting the P4, so this is
 * the primary observability surface (NFR-7). */
#ifndef UI_DIAG_OVERLAY
#define UI_DIAG_OVERLAY 1
#endif

namespace {

const char* TAG = "ui_shell";

constexpr int NUM_TABS = 3;
const char* kNavText[NUM_TABS] = {"NODES", "CHAT", "RADIO"};
const char* kNavIcon[NUM_TABS] = {LV_SYMBOL_LIST, LV_SYMBOL_KEYBOARD, LV_SYMBOL_WIFI};

enum NodeSort { SORT_HEARD = 0, SORT_SNR = 1, SORT_HOPS = 2 };
const char* kSortText[3] = {"Heard", "SNR", "Hops"};

struct ShellState {
    lv_obj_t* nav[NUM_TABS]   = {};
    lv_obj_t* panel[NUM_TABS] = {};
    int       active          = 0;

    /* status-bar widgets driven by the snapshot */
    lv_obj_t* my_badge = nullptr;
    lv_obj_t* my_long  = nullptr;
    lv_obj_t* my_id    = nullptr;
    lv_obj_t* conn_lbl = nullptr;
    lv_obj_t* count_lbl = nullptr;
    lv_obj_t* diag_lbl = nullptr;   /* bottom diagnostics strip */

    /* nodes tab */
    lv_obj_t* nodes_list     = nullptr;
    lv_obj_t* nodes_count    = nullptr;
    lv_obj_t* sort_chip[3]   = {};
    NodeSort  sort           = SORT_HEARD;
    uint32_t  last_gen       = 0xffffffff;
    NodeSort  last_sort      = SORT_HEARD;
    int64_t   last_build_us  = 0;
};
ShellState S;

/* scratch copy for rebuilds — file static so it never lands on a task stack */
node_rec_t g_nodes[APP_MAX_NODES];

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

/* Update label text only when it actually changes — a no-op set still
 * invalidates and churns redraws (NFR-3). */
void set_text(lv_obj_t* l, const char* t)
{
    if (!l) return;
    const char* cur = lv_label_get_text(l);
    if (cur && strcmp(cur, t) == 0) return;
    lv_label_set_text(l, t);
}

void set_color(lv_obj_t* l, uint32_t c)
{
    if (l) lv_obj_set_style_text_color(l, lv_color_hex(c), 0);
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

/* ---- node rows ---- */

/* Signal bucket → lit bar count + color (PRD §8). */
void signal_bucket(float snr, int* bars, uint32_t* color)
{
    if (snr >= -5)       { *bars = 4; *color = C_GREEN; }
    else if (snr >= -12) { *bars = 3; *color = C_GREEN; }
    else if (snr >= -17) { *bars = 2; *color = C_AMBER; }
    else                 { *bars = 1; *color = C_RED; }
}

/* Relative last-heard (no synced wall clock — PRD R2). */
void fmt_age(int64_t age_us, char* out, size_t cap)
{
    long long s = age_us / 1000000;
    if (s < 0) s = 0;
    if      (s < 10)    snprintf(out, cap, "now");
    else if (s < 60)    snprintf(out, cap, "%llds", s);
    else if (s < 3600)  snprintf(out, cap, "%lldm", s / 60);
    else if (s < 86400) snprintf(out, cap, "%lldh", s / 3600);
    else                snprintf(out, cap, "%lldd", s / 86400);
}

int cmp_nodes(const void* a, const void* b)
{
    const node_rec_t* x = (const node_rec_t*)a;
    const node_rec_t* y = (const node_rec_t*)b;
    switch (S.sort) {
    case SORT_SNR:
        if (x->snr < y->snr) return 1;
        if (x->snr > y->snr) return -1;
        return 0;
    case SORT_HOPS:
        if (x->hops != y->hops) return x->hops - y->hops;        /* fewer first */
        break;                                                   /* tie: by heard */
    case SORT_HEARD:
    default:
        break;
    }
    /* default / tie-break: most-recently-heard first */
    if (x->last_heard_us < y->last_heard_us) return 1;
    if (x->last_heard_us > y->last_heard_us) return -1;
    return 0;
}

void add_node_row(lv_obj_t* parent, const node_rec_t* n, int64_t now_us)
{
    lv_obj_t* row = box(parent, lv_pct(100), M_ROW_H);
    flex_row(row);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    hairline_side(row, LV_BORDER_SIDE_BOTTOM);

    /* badge */
    lv_obj_t* badge = box(row, 46, 46);
    bg(badge, C_SURF);
    radius(badge, M_RAD_M);
    const char* sn = (n->has_user && n->short_name[0]) ? n->short_name : "?";
    lv_obj_center(label(badge, sn, FONT_BODY, n->has_user ? C_HI : C_MID));

    /* name + id */
    lv_obj_t* mid = box(row, 0, 46);
    lv_obj_set_flex_grow(mid, 1);
    flex_col(mid);
    lv_obj_set_flex_align(mid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    label(mid, n->has_user ? n->long_name : "(unknown node)", FONT_ROW, C_HI);
    char id[20];
    snprintf(id, sizeof(id), "!%08lx", (unsigned long)n->num);
    label(mid, id, FONT_META, C_DIM);

    /* signal bars */
    int bars; uint32_t scol;
    signal_bucket(n->snr, &bars, &scol);
    lv_obj_t* sig = box(row, 24, 20);
    flex_row(sig);
    lv_obj_set_flex_align(sig, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_set_style_pad_column(sig, 2, 0);
    const int hgt[4] = {6, 9, 12, 16};
    for (int b = 0; b < 4; b++) {
        lv_obj_t* bar = box(sig, 4, hgt[b]);
        bg(bar, b < bars ? scol : C_HAIRLINE);
        radius(bar, 1);
    }

    /* numeric dB */
    char snr[12];
    snprintf(snr, sizeof(snr), "%.0f", (double)n->snr);
    lv_obj_t* sl = label(row, snr, FONT_BODY, scol);
    lv_obj_set_width(sl, 40);

    /* hop pill */
    lv_obj_t* pill = box(row, 0, 26);
    lv_obj_set_width(pill, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(pill, 9, 0);
    radius(pill, M_RAD_PILL);
    bg(pill, C_SURF);
    flex_row(pill);
    char hop[16];
    if (n->hops == 0) snprintf(hop, sizeof(hop), "DIRECT");
    else              snprintf(hop, sizeof(hop), "%d hops", n->hops);
    lv_obj_center(label(pill, hop, FONT_META, n->hops == 0 ? C_GREEN : C_MID));

    /* last heard */
    char age[12];
    fmt_age(now_us - n->last_heard_us, age, sizeof(age));
    lv_obj_t* al = label(row, age, FONT_META, C_DIM);
    lv_obj_set_width(al, 48);
    lv_obj_set_style_text_align(al, LV_TEXT_ALIGN_RIGHT, 0);
}

void rebuild_nodes(int64_t now_us)
{
    if (!S.nodes_list) return;
    uint32_t n = app_state_copy_nodes(g_nodes, APP_MAX_NODES);
    qsort(g_nodes, n, sizeof(node_rec_t), cmp_nodes);

    lv_obj_clean(S.nodes_list);
    for (uint32_t i = 0; i < n; i++) add_node_row(S.nodes_list, &g_nodes[i], now_us);

    if (S.nodes_count) {
        char c[24];
        snprintf(c, sizeof(c), "%lu heard", (unsigned long)n);
        set_text(S.nodes_count, c);
    }
}

void style_sort_chips(void)
{
    for (int i = 0; i < 3; i++)
        set_color(S.sort_chip[i], i == (int)S.sort ? C_GREEN : C_DIM);
}

void sort_cb(lv_event_t* e)
{
    S.sort = (NodeSort)(intptr_t)lv_event_get_user_data(e);
    style_sort_chips();   /* rebuild happens on the next refresh tick */
}

/* ---- snapshot refresh (LVGL task) ---- */

void refresh_cb(lv_timer_t*)
{
    app_snapshot_t s;
    app_state_snapshot(&s);

    /* my-node badge + identity */
    set_text(S.my_badge, s.my_short[0] ? s.my_short : "--");
    set_text(S.my_long, s.my_long[0] ? s.my_long : "No device");
    char id[20];
    if (s.my_num) snprintf(id, sizeof(id), "!%08lx", (unsigned long)s.my_num);
    else          snprintf(id, sizeof(id), "not connected");
    set_text(S.my_id, id);

    /* link-state chip */
    bool linked = conn_is_linked(s.state);
    set_text(S.conn_lbl, s.stage);
    set_color(S.conn_lbl, linked ? C_GREEN : (s.state == CONN_READY ? C_GREEN : C_AMBER));

    /* node count */
    char cnt[12];
    snprintf(cnt, sizeof(cnt), "%lu", (unsigned long)s.node_count);
    set_text(S.count_lbl, cnt);

#if UI_DIAG_OVERLAY
    if (S.diag_lbl) {
        const diag_t* d = &s.diag;
        char wc = d->wc_acked ? 'Y' : (d->wc_sent ? 'q' : 'n');
        char line[176];
        snprintf(line, sizeof(line),
                 "%s  mtu%u ci%u  rd%lu ni%lu df%lu nf%lu pl%lu rt%lu  su%c wc%c wr%lu err%d",
                 s.stage, (unsigned)d->mtu, (unsigned)d->conn_itvl,
                 (unsigned long)d->reads, (unsigned long)d->nodeinfo,
                 (unsigned long)d->decfail, (unsigned long)d->notifies,
                 (unsigned long)d->polls, (unsigned long)d->read_tmos,
                 d->cccd_ok ? 'Y' : 'n', wc, (unsigned long)d->wc_retries, d->last_err);
        set_text(S.diag_lbl, line);
    }
#endif

    /* Rebuild the node list only on a meaningful change or a sort change — never
     * on SNR jitter (FR-3.2). While the Nodes tab is visible, also rebuild every
     * 3 s so the relative last-heard ages stay live. */
    uint32_t gen   = app_state_nodes_gen();
    int64_t  now   = esp_timer_get_time();
    bool     view  = (S.active == 0);
    bool     aged  = view && (now - S.last_build_us > 3000000);
    if (gen != S.last_gen || S.sort != S.last_sort || aged) {
        rebuild_nodes(now);
        S.last_gen      = gen;
        S.last_sort     = S.sort;
        S.last_build_us = now;
    }
}

/* The live Nodes tab: header (title + count + sort chips) over a scrolling list. */
lv_obj_t* make_nodes_panel(lv_obj_t* parent)
{
    lv_obj_t* panel = box(parent, lv_pct(100), lv_pct(100));
    flex_col(panel);

    lv_obj_t* hdr = box(panel, lv_pct(100), 54);
    flex_row(hdr);
    lv_obj_set_style_pad_hor(hdr, 20, 0);
    lv_obj_set_style_pad_column(hdr, 14, 0);
    hairline_side(hdr, LV_BORDER_SIDE_BOTTOM);
    label(hdr, "Mesh nodes", FONT_ROW, C_HI);
    S.nodes_count = label(hdr, "0 heard", FONT_META, C_DIM);

    lv_obj_t* spacer = box(hdr, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);

    label(hdr, "sort", FONT_META, C_DIM);
    for (int i = 0; i < 3; i++) {
        lv_obj_t* chip = box(hdr, 0, 30);
        lv_obj_set_width(chip, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_hor(chip, 9, 0);
        radius(chip, M_RAD_PILL);
        bg(chip, C_SURF);
        flex_row(chip);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(chip, sort_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        S.sort_chip[i] = label(chip, kSortText[i], FONT_META, i == 0 ? C_GREEN : C_DIM);
    }

    lv_obj_t* list = box(panel, lv_pct(100), 0);
    lv_obj_set_flex_grow(list, 1);
    flex_col(list);
    lv_obj_set_style_pad_hor(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 12, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    S.nodes_list = list;

    return panel;
}

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
    S.my_badge = label(mb, "--", FONT_META, C_MID);
    lv_obj_center(S.my_badge);

    lv_obj_t* me = box(sb, 0, 40);
    lv_obj_set_width(me, LV_SIZE_CONTENT);
    flex_col(me);
    S.my_long = label(me, "No device", FONT_META, C_HI);
    S.my_id   = label(me, "not connected", FONT_META, C_DIM);

    lv_obj_t* spacer = box(sb, 0, 1);
    lv_obj_set_flex_grow(spacer, 1);

    lv_obj_t* chip = box(sb, 0, 32);
    lv_obj_set_width(chip, LV_SIZE_CONTENT);
    flex_row(chip);
    lv_obj_set_style_pad_hor(chip, 13, 0);
    lv_obj_set_style_pad_column(chip, 9, 0);
    radius(chip, M_RAD_M);
    lv_obj_set_style_border_width(chip, 1, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(C_SURF), 0);
    S.conn_lbl = label(chip, "BOOT", FONT_META, C_AMBER);

    lv_obj_t* cnt = box(sb, 0, 32);
    lv_obj_set_width(cnt, LV_SIZE_CONTENT);
    flex_row(cnt);
    lv_obj_set_style_pad_column(cnt, 7, 0);
    label(cnt, LV_SYMBOL_WIFI, FONT_META, C_MID);
    S.count_lbl = label(cnt, "0", FONT_META, C_MID);

    label(sb, "--:--", FONT_BODY, C_HI);

    /* content area + panels */
    lv_obj_t* content = box(col, lv_pct(100), 0);
    lv_obj_set_flex_grow(content, 1);
    bg(content, C_BG);

    S.panel[0] = make_nodes_panel(content);
    S.panel[1] = make_panel(content, "Primary  -  broadcast", "Chat lands in M4");
    S.panel[2] = make_panel(content, "Radio", "Onboarding / device picker lands in M5");

#if UI_DIAG_OVERLAY
    /* diagnostics strip pinned to the bottom of the main column */
    lv_obj_t* diag = box(col, lv_pct(100), 22);
    bg(diag, C_CHROME);
    lv_obj_set_style_pad_hor(diag, 10, 0);
    hairline_side(diag, LV_BORDER_SIDE_TOP);
    S.diag_lbl = label(diag, "diag", FONT_META, C_DIM);
    lv_obj_center(S.diag_lbl);
#endif

    set_tab(0);

    /* the BLE→UI snapshot pump (LVGL task) */
    lv_timer_create(refresh_cb, 500, nullptr);

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
