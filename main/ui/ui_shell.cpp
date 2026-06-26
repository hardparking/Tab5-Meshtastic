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
#include "settings.h"
#include "ble_transport.h"
#include "keyboard.h"

#include "lvgl.h"
#include "lvgl_port.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <ctype.h>
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
    lv_obj_t* diag_box = nullptr;   /* its container (toggled)   */
    bool      diag_on  = false;     /* hidden by default (release look) */

    /* nodes tab */
    lv_obj_t* nodes_list     = nullptr;
    lv_obj_t* nodes_count    = nullptr;
    lv_obj_t* sort_chip[3]   = {};
    NodeSort  sort           = SORT_HEARD;
    uint32_t  last_gen       = 0xffffffff;
    NodeSort  last_sort      = SORT_HEARD;
    int64_t   last_rebuild_us = 0;      /* throttles full rebuilds during the sync flood */

    /* node detail view (overlays the content area) */
    lv_obj_t* detail      = nullptr;
    bool      detail_open = false;
    uint32_t  detail_num  = 0;
    lv_obj_t* d_badge = nullptr;
    lv_obj_t* d_name  = nullptr;
    lv_obj_t* d_id    = nullptr;
    lv_obj_t* d_snr   = nullptr;
    lv_obj_t* d_hops  = nullptr;
    lv_obj_t* d_heard = nullptr;
    lv_obj_t* d_batt  = nullptr;
    lv_obj_t* d_pos   = nullptr;
    lv_obj_t* d_volt  = nullptr;
    lv_obj_t* d_chan  = nullptr;
    lv_obj_t* d_air   = nullptr;
    lv_obj_t* d_uptime = nullptr;

    /* chat tab */
    lv_obj_t* chat_list  = nullptr;   /* scrolling bubble area   */
    lv_obj_t* chat_input = nullptr;   /* composer textarea       */
    lv_obj_t* chat_kb    = nullptr;   /* on-screen keyboard      */
    uint32_t  msg_seen   = 0;         /* bubbles already rendered */

    /* radio tab — onboarding / device picker */
    lv_obj_t* v_manager  = nullptr;   /* saved devices + scan button */
    lv_obj_t* v_disco    = nullptr;   /* discovery scan list         */
    lv_obj_t* v_pin      = nullptr;   /* PIN keypad                  */
    lv_obj_t* mgr_list   = nullptr;
    lv_obj_t* mgr_status = nullptr;
    lv_obj_t* disco_list = nullptr;
    lv_obj_t* pin_disp   = nullptr;
    lv_obj_t* pin_dev    = nullptr;   /* name of the device being paired */
    int       radio_view = 0;         /* 0 manager, 1 discovery, 2 pin */
    char      pin_name[32] = {};
    char      pin_buf[8]  = {};
    uint32_t  last_scan_gen = 0xffffffff;
};
ShellState S;

/* scratch copy for rebuilds — file static so it never lands on a task stack */
node_rec_t g_nodes[APP_MAX_NODES];

/* Stable per-row widget handles so last-heard ages and SNR can be refreshed in
 * place every tick without rebuilding the row (rebuild only on add/remove/sort).
 * Parallel to the rows currently in nodes_list, in display order. */
struct NodeRow {
    uint32_t  num;
    lv_obj_t* snr_lbl;
    lv_obj_t* bars[4];
    lv_obj_t* age_lbl;
    int       bucket;   /* last-rendered signal bucket (to gate bar restyle) */
};
NodeRow  g_rows[APP_MAX_NODES];
uint32_t g_row_n = 0;

/* UI-side copies for the radio tab (index → addr lookups for row callbacks) */
saved_device_t g_mgr[SETTINGS_MAX_SAVED];
uint32_t       g_mgr_n = 0;
scan_result_t  g_disco[APP_MAX_SCAN];
uint32_t       g_disco_n = 0;

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
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* interactive elements opt in */
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
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

/* forward decls: detail view + chat + radio (defined after the nodes panel) */
void open_detail(uint32_t num);
void close_detail(void);
void populate_detail(void);
void append_messages(void);
void radio_refresh(void);
void rebuild_manager(void);
void rebuild_discovery(void);
void radio_show(int view);

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
    close_detail();   /* leaving to another tab dismisses the node detail */
    if (S.chat_kb) lv_obj_add_flag(S.chat_kb, LV_OBJ_FLAG_HIDDEN);   /* hide OSK */
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
    if (i == 2 && S.v_manager) radio_show(0);   /* land on the device manager */
}

void nav_cb(lv_event_t* e) { set_tab((int)(intptr_t)lv_event_get_user_data(e)); }

/* Long-press the status bar to reveal/hide the on-screen diagnostics strip
 * (FR-5.1: hidden in "release", summonable on-device since serial resets the P4). */
void diag_toggle_cb(lv_event_t*)
{
    if (!S.diag_box) return;
    S.diag_on = !S.diag_on;
    (S.diag_on ? lv_obj_clear_flag : lv_obj_add_flag)(S.diag_box, LV_OBJ_FLAG_HIDDEN);
}

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

void style_bars(lv_obj_t* const bars[4], int lit, uint32_t color)
{
    for (int b = 0; b < 4; b++) bg(bars[b], b < lit ? color : C_HAIRLINE);
}

void row_click_cb(lv_event_t* e)
{
    open_detail((uint32_t)(intptr_t)lv_event_get_user_data(e));
}

void add_node_row(lv_obj_t* parent, const node_rec_t* n, int64_t now_us, NodeRow* out)
{
    lv_obj_t* row = box(parent, lv_pct(100), M_ROW_H);
    flex_row(row);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_column(row, 12, 0);
    hairline_side(row, LV_BORDER_SIDE_BOTTOM);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, row_click_cb, LV_EVENT_CLICKED, (void*)(intptr_t)n->num);

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
        out->bars[b] = bar;
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

    out->num     = n->num;
    out->snr_lbl = sl;
    out->age_lbl = al;
    out->bucket  = bars;
}

/* Refresh last-heard ages + SNR on the existing rows without rebuilding them
 * (FR-3.2: no full repaint on SNR jitter). Matches rows to the live DB by num;
 * restyles the bars only when the signal bucket actually crosses a threshold. */
void refresh_node_rows(int64_t now_us)
{
    uint32_t n = app_state_copy_nodes(g_nodes, APP_MAX_NODES);
    for (uint32_t i = 0; i < g_row_n; i++) {
        NodeRow* r = &g_rows[i];
        const node_rec_t* nd = nullptr;
        for (uint32_t j = 0; j < n; j++)
            if (g_nodes[j].num == r->num) { nd = &g_nodes[j]; break; }
        if (!nd) continue;

        char age[12];
        fmt_age(now_us - nd->last_heard_us, age, sizeof(age));
        set_text(r->age_lbl, age);

        int bars; uint32_t scol;
        signal_bucket(nd->snr, &bars, &scol);
        char snr[12];
        snprintf(snr, sizeof(snr), "%.0f", (double)nd->snr);
        set_text(r->snr_lbl, snr);
        if (bars != r->bucket) {            /* bucket crossed — restyle bars + dB color */
            style_bars(r->bars, bars, scol);
            set_color(r->snr_lbl, scol);
            r->bucket = bars;
        }
    }
}

void rebuild_nodes(int64_t now_us)
{
    if (!S.nodes_list) return;
    uint32_t n = app_state_copy_nodes(g_nodes, APP_MAX_NODES);
    qsort(g_nodes, n, sizeof(node_rec_t), cmp_nodes);

    int32_t scroll_y = lv_obj_get_scroll_y(S.nodes_list);   /* preserve view */
    lv_obj_clean(S.nodes_list);
    g_row_n = 0;
    for (uint32_t i = 0; i < n && i < APP_MAX_NODES; i++)
        add_node_row(S.nodes_list, &g_nodes[i], now_us, &g_rows[g_row_n++]);
    lv_obj_scroll_to_y(S.nodes_list, scroll_y, LV_ANIM_OFF);

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
    if (S.diag_lbl && S.diag_on) {
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

    /* Full rebuild (clean + recreate rows) only on add/remove/sort — never on
     * SNR jitter (FR-3.2) or a timer — and only while the Nodes tab is actually
     * the visible view. A hidden list never needs repainting, and rebuilding it
     * off-tab just burns the LVGL task.
     *
     * During CONN_SYNCING the radio streams NodeInfo faster than a human can
     * read, so nodes_gen changes on essentially every tick. A full clean +
     * recreate (~13 LVGL objects/row) each tick monopolizes the single LVGL
     * task and starves touch — the UI feels frozen. Rate-limit rebuilds while
     * syncing; the list still catches up each window, and the cheap in-place
     * age/SNR refresh keeps the already-shown rows live in between. */
    uint32_t gen = app_state_nodes_gen();
    int64_t  now = esp_timer_get_time();
    if (S.active == 0 && !S.detail_open) {
        bool    dirty   = (gen != S.last_gen || S.sort != S.last_sort);
        int64_t min_gap = (s.state == CONN_SYNCING) ? 2000000 : 0;   /* 2 s during the flood */
        if (dirty && now - S.last_rebuild_us >= min_gap) {
            rebuild_nodes(now);
            S.last_gen        = gen;
            S.last_sort       = S.sort;
            S.last_rebuild_us = now;
        } else {
            refresh_node_rows(now);
        }
    }

    /* keep the open detail view live (SNR / last-heard / telemetry) */
    if (S.detail_open) populate_detail();

    /* chat: append any newly-arrived messages (snappy path, FR-4.3) */
    append_messages();

    /* radio tab: live status + discovery results */
    if (S.active == 2) radio_refresh();
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

/* ---- node detail view (PRD §6.3 FR-3.4) ---- */

void fmt_uptime(uint32_t s, char* out, size_t cap)
{
    if      (s >= 86400) snprintf(out, cap, "%lud %luh", (unsigned long)(s / 86400), (unsigned long)((s % 86400) / 3600));
    else if (s >= 3600)  snprintf(out, cap, "%luh %lum", (unsigned long)(s / 3600), (unsigned long)((s % 3600) / 60));
    else if (s >= 60)    snprintf(out, cap, "%lum", (unsigned long)(s / 60));
    else                 snprintf(out, cap, "%lus", (unsigned long)s);
}

/* A stat card: small dim title over a big value. Returns the value label. */
lv_obj_t* make_card(lv_obj_t* parent, const char* title)
{
    lv_obj_t* card = box(parent, 0, 84);
    lv_obj_set_flex_grow(card, 1);
    bg(card, C_SURF2);
    radius(card, M_RAD_M);
    flex_col(card);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_pad_row(card, 6, 0);
    label(card, title, FONT_META, C_DIM);
    return label(card, "--", FONT_TITLE, C_HI);
}

/* A "label: value" metrics line. Returns the value label. */
lv_obj_t* make_metric(lv_obj_t* parent, const char* title)
{
    lv_obj_t* row = box(parent, lv_pct(100), 34);
    flex_row(row);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_t* t = label(row, title, FONT_BODY, C_MID);
    lv_obj_set_width(t, 180);
    return label(row, "--", FONT_BODY, C_HI);
}

void back_cb(lv_event_t*) { close_detail(); }

lv_obj_t* make_detail_panel(lv_obj_t* parent)
{
    lv_obj_t* panel = box(parent, lv_pct(100), lv_pct(100));
    bg(panel, C_BG);
    flex_col(panel);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_HIDDEN);

    /* header: back + badge + name/id */
    lv_obj_t* hdr = box(panel, lv_pct(100), 64);
    flex_row(hdr);
    lv_obj_set_style_pad_hor(hdr, 16, 0);
    lv_obj_set_style_pad_column(hdr, 14, 0);
    hairline_side(hdr, LV_BORDER_SIDE_BOTTOM);

    lv_obj_t* back = box(hdr, 44, 44);
    bg(back, C_SURF);
    radius(back, M_RAD_M);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back, back_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(label(back, LV_SYMBOL_LEFT, FONT_BODY, C_HI));

    lv_obj_t* badge = box(hdr, 44, 44);
    bg(badge, C_SURF);
    radius(badge, M_RAD_M);
    S.d_badge = label(badge, "?", FONT_ROW, C_HI);
    lv_obj_center(S.d_badge);

    lv_obj_t* idcol = box(hdr, 0, 48);
    lv_obj_set_flex_grow(idcol, 1);
    flex_col(idcol);
    lv_obj_set_flex_align(idcol, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    S.d_name = label(idcol, "--", FONT_TITLE, C_HI);
    S.d_id   = label(idcol, "--", FONT_META, C_DIM);

    /* body (scrollable in case of overflow) */
    lv_obj_t* body = box(panel, lv_pct(100), 0);
    lv_obj_set_flex_grow(body, 1);
    flex_col(body);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_style_pad_row(body, 14, 0);
    lv_obj_add_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(body, LV_DIR_VER);

    /* stat cards */
    lv_obj_t* cards = box(body, lv_pct(100), 84);
    flex_row(cards);
    lv_obj_set_style_pad_column(cards, 12, 0);
    S.d_snr   = make_card(cards, "SNR");
    S.d_hops  = make_card(cards, "HOPS");
    S.d_heard = make_card(cards, "LAST HEARD");
    S.d_batt  = make_card(cards, "BATTERY");

    /* position */
    lv_obj_t* pos = box(body, lv_pct(100), 0);
    lv_obj_set_height(pos, LV_SIZE_CONTENT);
    bg(pos, C_SURF2);
    radius(pos, M_RAD_M);
    flex_col(pos);
    lv_obj_set_style_pad_all(pos, 14, 0);
    lv_obj_set_style_pad_row(pos, 6, 0);
    label(pos, "POSITION", FONT_META, C_DIM);
    S.d_pos = label(pos, "--", FONT_ROW, C_HI);

    /* device metrics */
    lv_obj_t* met = box(body, lv_pct(100), 0);
    lv_obj_set_height(met, LV_SIZE_CONTENT);
    bg(met, C_SURF2);
    radius(met, M_RAD_M);
    flex_col(met);
    lv_obj_set_style_pad_all(met, 14, 0);
    lv_obj_set_style_pad_row(met, 4, 0);
    label(met, "DEVICE METRICS", FONT_META, C_DIM);
    S.d_volt   = make_metric(met, "Voltage");
    S.d_chan   = make_metric(met, "Channel util");
    S.d_air    = make_metric(met, "Tx airtime");
    S.d_uptime = make_metric(met, "Uptime");

    return panel;
}

void populate_detail(void)
{
    node_rec_t n;
    if (!app_state_get_node(S.detail_num, &n)) return;

    set_text(S.d_badge, (n.has_user && n.short_name[0]) ? n.short_name : "?");
    set_text(S.d_name, n.has_user ? n.long_name : "(unknown node)");
    char id[20];
    snprintf(id, sizeof(id), "!%08lx", (unsigned long)n.num);
    set_text(S.d_id, id);

    /* cards */
    int bars; uint32_t scol;
    signal_bucket(n.snr, &bars, &scol);
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", (double)n.snr);
    set_text(S.d_snr, buf);
    set_color(S.d_snr, scol);

    if (!n.hops_valid)      set_text(S.d_hops, "?");
    else if (n.hops == 0)   set_text(S.d_hops, "DIRECT");
    else { snprintf(buf, sizeof(buf), "%d", n.hops); set_text(S.d_hops, buf); }
    set_color(S.d_hops, n.hops_valid && n.hops == 0 ? C_GREEN : C_HI);

    fmt_age(esp_timer_get_time() - n.last_heard_us, buf, sizeof(buf));
    set_text(S.d_heard, buf);

    if (n.has_metrics && n.metrics.has_batt) {
        if (n.metrics.batt > 100) set_text(S.d_batt, "PWR");
        else { snprintf(buf, sizeof(buf), "%lu%%", (unsigned long)n.metrics.batt); set_text(S.d_batt, buf); }
    } else set_text(S.d_batt, "--");

    /* position */
    if (n.has_position && n.position.has_loc) {
        char pos[80];
        int len = snprintf(pos, sizeof(pos), "%.5f, %.5f",
                           n.position.lat_i * 1e-7, n.position.lon_i * 1e-7);
        if (n.position.has_alt)
            len += snprintf(pos + len, sizeof(pos) - len, "   alt %ldm", (long)n.position.alt_m);
        if (n.position.sats)
            snprintf(pos + len, sizeof(pos) - len, "   %lu sats", (unsigned long)n.position.sats);
        set_text(S.d_pos, pos);
    } else {
        set_text(S.d_pos, "No position reported");
    }

    /* metrics */
    const mesh_metrics_t* m = &n.metrics;
    if (n.has_metrics && m->has_volt)     { snprintf(buf, sizeof(buf), "%.2f V", (double)m->volt); set_text(S.d_volt, buf); }
    else set_text(S.d_volt, "--");
    if (n.has_metrics && m->has_chanutil) { snprintf(buf, sizeof(buf), "%.1f %%", (double)m->chan_util); set_text(S.d_chan, buf); }
    else set_text(S.d_chan, "--");
    if (n.has_metrics && m->has_airtx)    { snprintf(buf, sizeof(buf), "%.1f %%", (double)m->air_tx); set_text(S.d_air, buf); }
    else set_text(S.d_air, "--");
    if (n.has_metrics && m->has_uptime)   { fmt_uptime(m->uptime, buf, sizeof(buf)); set_text(S.d_uptime, buf); }
    else set_text(S.d_uptime, "--");
}

void open_detail(uint32_t num)
{
    S.detail_num  = num;
    S.detail_open = true;
    populate_detail();
    if (S.detail) lv_obj_clear_flag(S.detail, LV_OBJ_FLAG_HIDDEN);
}

void close_detail(void)
{
    S.detail_open = false;
    if (S.detail) lv_obj_add_flag(S.detail, LV_OBJ_FLAG_HIDDEN);
}

/* ---- chat tab (PRD §6.4) ---- */

/* Resolve a sender's short display name from the node DB; hex fallback. */
void sender_name(uint32_t num, char* out, size_t cap)
{
    node_rec_t n;
    if (app_state_get_node(num, &n) && n.has_user && n.short_name[0])
        snprintf(out, cap, "%s", n.short_name);
    else
        snprintf(out, cap, "!%04lx", (unsigned long)(num & 0xffff));
}

void add_bubble(lv_obj_t* parent, const msg_rec_t* m)
{
    lv_obj_t* row = box(parent, lv_pct(100), LV_SIZE_CONTENT);
    flex_row(row);
    /* self → right, received → left */
    lv_obj_set_flex_align(row, m->is_self ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t* col = box(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    flex_col(col);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START,
                          m->is_self ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          m->is_self ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, 2, 0);

    if (!m->is_self) {
        char who[16];
        sender_name(m->from, who, sizeof(who));
        label(col, who, FONT_META, C_MID);
    }

    lv_obj_t* bub = box(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    bg(bub, m->is_self ? C_GREEN : C_SURF);
    radius(bub, M_RAD_M);
    lv_obj_set_style_pad_hor(bub, 12, 0);
    lv_obj_set_style_pad_ver(bub, 8, 0);
    lv_obj_t* txt = label(bub, m->text, FONT_BODY, m->is_self ? C_INK : C_HI);
    if (strlen(m->text) > 28) {                  /* wrap long messages */
        lv_obj_set_width(txt, 680);
        lv_label_set_long_mode(txt, LV_LABEL_LONG_MODE_WRAP);
    }
}

/* Append only newly-arrived messages (FR-4.3: no whole-list rebuild on the
 * steady-state path) and scroll to the newest. */
void append_messages(void)
{
    uint32_t total = app_state_msg_total();
    if (total == S.msg_seen || !S.chat_list) return;

    static msg_rec_t buf[APP_MAX_MSGS];
    uint32_t n         = app_state_copy_messages(buf, APP_MAX_MSGS);
    uint32_t new_count = total - S.msg_seen;
    if (new_count > n) {                          /* first load or dropped gap */
        lv_obj_clean(S.chat_list);
        for (uint32_t i = 0; i < n; i++) add_bubble(S.chat_list, &buf[i]);
    } else {
        for (uint32_t i = n - new_count; i < n; i++) add_bubble(S.chat_list, &buf[i]);
    }
    S.msg_seen = total;

    uint32_t cnt = lv_obj_get_child_count(S.chat_list);
    if (cnt) lv_obj_scroll_to_view(lv_obj_get_child(S.chat_list, cnt - 1), LV_ANIM_OFF);
}

void do_send(void)
{
    const char* t = lv_textarea_get_text(S.chat_input);
    if (!t || !t[0]) return;
    ble_transport_send_text(t);          /* transmits + local-echoes into the log */
    lv_textarea_set_text(S.chat_input, "");
}

void ta_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_FOCUSED) {
        /* With a physical keyboard attached, the on-screen keyboard is just
         * clutter — only summon it as a touch-only fallback. */
        if (kbd_present()) return;
        lv_keyboard_set_textarea(S.chat_kb, S.chat_input);
        lv_obj_clear_flag(S.chat_kb, LV_OBJ_FLAG_HIDDEN);
    } else if (code == LV_EVENT_DEFOCUSED) {
        lv_obj_add_flag(S.chat_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

void kb_event_cb(lv_event_t* e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY) {                 /* checkmark = send */
        do_send();
    } else if (code == LV_EVENT_CANCEL) {
        lv_obj_add_flag(S.chat_kb, LV_OBJ_FLAG_HIDDEN);
    }
}

void send_btn_cb(lv_event_t*) { do_send(); }

lv_obj_t* make_chat_panel(lv_obj_t* parent)
{
    lv_obj_t* panel = box(parent, lv_pct(100), lv_pct(100));
    flex_col(panel);

    lv_obj_t* hdr = box(panel, lv_pct(100), 44);
    flex_row(hdr);
    lv_obj_set_style_pad_hor(hdr, 20, 0);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    hairline_side(hdr, LV_BORDER_SIDE_BOTTOM);
    label(hdr, "Primary", FONT_ROW, C_HI);
    label(hdr, "broadcast", FONT_META, C_DIM);

    lv_obj_t* list = box(panel, lv_pct(100), 0);
    lv_obj_set_flex_grow(list, 1);
    flex_col(list);
    lv_obj_set_style_pad_hor(list, 16, 0);
    lv_obj_set_style_pad_ver(list, 12, 0);
    lv_obj_set_style_pad_row(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    S.chat_list = list;

    /* composer */
    lv_obj_t* comp = box(panel, lv_pct(100), 60);
    bg(comp, C_CHROME);
    flex_row(comp);
    lv_obj_set_style_pad_hor(comp, 12, 0);
    lv_obj_set_style_pad_column(comp, 10, 0);
    hairline_side(comp, LV_BORDER_SIDE_TOP);

    lv_obj_t* ta = lv_textarea_create(comp);
    lv_obj_set_flex_grow(ta, 1);
    lv_obj_set_height(ta, 44);
    lv_textarea_set_one_line(ta, true);
    lv_textarea_set_placeholder_text(ta, "Message the mesh...");
    lv_textarea_set_max_length(ta, 200);
    bg(ta, C_SURF2);
    radius(ta, M_RAD_M);
    lv_obj_set_style_border_width(ta, 0, 0);
    lv_obj_set_style_text_font(ta, FONT_BODY, 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_HI), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_DIM), LV_PART_TEXTAREA_PLACEHOLDER);
    lv_obj_set_style_anim_duration(ta, 0, LV_PART_CURSOR);   /* no blink (FR-4.4) */
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(ta, ta_event_cb, LV_EVENT_DEFOCUSED, nullptr);
    S.chat_input = ta;

    lv_obj_t* send = box(comp, 64, 44);
    bg(send, C_GREEN);
    radius(send, M_RAD_M);
    lv_obj_add_flag(send, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(send, send_btn_cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(label(send, LV_SYMBOL_OK, FONT_ROW, C_INK));

    /* on-screen keyboard — overlays from the bottom; shown only on focus */
    lv_obj_t* kb = lv_keyboard_create(lv_screen_active());
    lv_obj_set_size(kb, lv_pct(100), 320);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_READY, nullptr);
    lv_obj_add_event_cb(kb, kb_event_cb, LV_EVENT_CANCEL, nullptr);
    S.chat_kb = kb;

    return panel;
}

/* ---- radio tab: onboarding / device picker (PRD §6.1) ---- */

void fmt_addr(const uint8_t a[6], char* out, size_t cap)
{
    snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
}

/* A pill button. user_data passed to the click handler. */
lv_obj_t* make_btn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* ud,
                   uint32_t bgc, uint32_t txtc, int w, int h)
{
    lv_obj_t* b = box(parent, w, h);
    bg(b, bgc);
    radius(b, M_RAD_M);
    flex_row(b);
    lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (cb) {
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    }
    lv_obj_center(label(b, text, FONT_ROW, txtc));
    return b;
}

void radio_show(int view)
{
    S.radio_view = view;
    if (S.v_manager) (view == 0 ? lv_obj_clear_flag : lv_obj_add_flag)(S.v_manager, LV_OBJ_FLAG_HIDDEN);
    if (S.v_disco)   (view == 1 ? lv_obj_clear_flag : lv_obj_add_flag)(S.v_disco, LV_OBJ_FLAG_HIDDEN);
    if (S.v_pin)     (view == 2 ? lv_obj_clear_flag : lv_obj_add_flag)(S.v_pin, LV_OBJ_FLAG_HIDDEN);
    if (view == 0) rebuild_manager();
    else if (view == 1) rebuild_discovery();
}

/* ---- manager ---- */

void mgr_connect_cb(lv_event_t* e)
{
    uint32_t i = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    if (i < g_mgr_n) ble_transport_connect(g_mgr[i].addr, (uint32_t)atoi(g_mgr[i].pin));
    radio_show(0);
}

void mgr_forget_cb(lv_event_t* e)
{
    uint32_t i = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    if (i < g_mgr_n) ble_transport_forget(g_mgr[i].addr);
    rebuild_manager();
}

void scan_btn_cb(lv_event_t*)
{
    ble_transport_scan();
    radio_show(1);
}

void rebuild_manager(void)
{
    if (!S.mgr_list) return;
    lv_obj_clean(S.mgr_list);
    g_mgr_n = settings_get_saved(g_mgr, SETTINGS_MAX_SAVED);

    saved_device_t active;
    bool have_active = settings_get_active(&active);

    if (g_mgr_n == 0) {
        lv_obj_t* w = label(S.mgr_list, "No saved radios.\nTap \"Scan for devices\" to add one.",
                            FONT_BODY, C_DIM);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(w);
        return;
    }

    for (uint32_t i = 0; i < g_mgr_n; i++) {
        bool is_active = have_active && memcmp(active.addr, g_mgr[i].addr, 6) == 0;

        lv_obj_t* row = box(S.mgr_list, lv_pct(100), 64);
        flex_row(row);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        hairline_side(row, LV_BORDER_SIDE_BOTTOM);

        lv_obj_t* col = box(row, 0, 48);
        lv_obj_set_flex_grow(col, 1);
        flex_col(col);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        label(col, g_mgr[i].name[0] ? g_mgr[i].name : "Meshtastic", FONT_ROW, C_HI);
        char ad[20]; fmt_addr(g_mgr[i].addr, ad, sizeof(ad));
        label(col, ad, FONT_META, C_DIM);

        if (is_active)
            make_btn(row, "Active", nullptr, nullptr, C_SURF, C_GREEN, 90, 40);
        else
            make_btn(row, "Connect", mgr_connect_cb, (void*)(intptr_t)i, C_GREEN, C_INK, 96, 40);
        make_btn(row, "Forget", mgr_forget_cb, (void*)(intptr_t)i, C_SURF, C_RED, 84, 40);
    }
}

/* ---- discovery ---- */

void disco_row_cb(lv_event_t* e)
{
    uint32_t i = (uint32_t)(intptr_t)lv_event_get_user_data(e);
    if (i >= g_disco_n) return;
    snprintf(S.pin_name, sizeof(S.pin_name), "%s", g_disco[i].name);
    S.pin_buf[0] = 0;

    char pin[8];
    if (g_disco[i].saved && settings_is_saved(g_disco[i].addr, pin)) {
        ble_transport_connect(g_disco[i].addr, (uint32_t)atoi(pin));  /* known PIN */
    } else {
        ble_transport_connect(g_disco[i].addr, 0);   /* prompt when the radio asks */
    }
    radio_show(0);   /* the PIN keypad appears via CONN_ENTER_PIN if needed */
}

void rescan_cb(lv_event_t*) { ble_transport_scan(); rebuild_discovery(); }
void disco_back_cb(lv_event_t*) { radio_show(0); }

void rebuild_discovery(void)
{
    if (!S.disco_list) return;
    lv_obj_clean(S.disco_list);
    g_disco_n = app_state_copy_scan(g_disco, APP_MAX_SCAN);

    if (g_disco_n == 0) {
        lv_obj_t* w = label(S.disco_list, "Scanning for Meshtastic radios...", FONT_BODY, C_DIM);
        lv_obj_center(w);
        return;
    }

    for (uint32_t i = 0; i < g_disco_n; i++) {
        lv_obj_t* row = box(S.disco_list, lv_pct(100), 60);
        flex_row(row);
        lv_obj_set_style_pad_hor(row, 12, 0);
        lv_obj_set_style_pad_column(row, 10, 0);
        hairline_side(row, LV_BORDER_SIDE_BOTTOM);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, disco_row_cb, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t* col = box(row, 0, 46);
        lv_obj_set_flex_grow(col, 1);
        flex_col(col);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        label(col, g_disco[i].name, FONT_ROW, C_HI);
        char ad[20]; fmt_addr(g_disco[i].addr, ad, sizeof(ad));
        label(col, ad, FONT_META, C_DIM);

        if (g_disco[i].saved) {
            lv_obj_t* pill = box(row, 0, 26);
            lv_obj_set_width(pill, LV_SIZE_CONTENT);
            lv_obj_set_style_pad_hor(pill, 9, 0);
            radius(pill, M_RAD_PILL);
            bg(pill, C_SURF);
            flex_row(pill);
            lv_obj_center(label(pill, "paired", FONT_META, C_GREEN));
        }
        char rs[12]; snprintf(rs, sizeof(rs), "%d", g_disco[i].rssi);
        label(row, rs, FONT_META, C_MID);
    }
}

/* ---- PIN keypad ---- */

void update_pin_disp(void)
{
    char d[8];
    size_t len = strlen(S.pin_buf);
    for (int i = 0; i < 6; i++) d[i] = i < (int)len ? S.pin_buf[i] : '_';
    d[6] = 0;
    set_text(S.pin_disp, d);
}

void key_cb(lv_event_t* e)
{
    int v = (int)(intptr_t)lv_event_get_user_data(e);
    size_t len = strlen(S.pin_buf);
    if (v == 10) {                       /* backspace */
        if (len) S.pin_buf[len - 1] = 0;
    } else if (len < 6) {                /* digit */
        S.pin_buf[len] = (char)('0' + v);
        S.pin_buf[len + 1] = 0;
    }
    update_pin_disp();
}

void pin_connect_cb(lv_event_t*)
{
    if (S.pin_buf[0]) ble_transport_submit_pin((uint32_t)atoi(S.pin_buf));
    /* radio_refresh moves the view off the keypad as the state advances */
}

void pin_cancel_cb(lv_event_t*)
{
    ble_transport_cancel();
    radio_show(0);
}

lv_obj_t* make_radio_panel(lv_obj_t* parent)
{
    lv_obj_t* panel = box(parent, lv_pct(100), lv_pct(100));
    bg(panel, C_BG);

    /* --- manager view --- */
    lv_obj_t* mgr = box(panel, lv_pct(100), lv_pct(100));
    flex_col(mgr);
    S.v_manager = mgr;
    lv_obj_t* mh = box(mgr, lv_pct(100), 54);
    flex_row(mh);
    lv_obj_set_style_pad_hor(mh, 20, 0);
    lv_obj_set_style_pad_column(mh, 12, 0);
    hairline_side(mh, LV_BORDER_SIDE_BOTTOM);
    label(mh, "Radios", FONT_ROW, C_HI);
    S.mgr_status = label(mh, "", FONT_META, C_DIM);
    lv_obj_t* mlist = box(mgr, lv_pct(100), 0);
    lv_obj_set_flex_grow(mlist, 1);
    flex_col(mlist);
    lv_obj_set_style_pad_hor(mlist, 8, 0);
    lv_obj_add_flag(mlist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(mlist, LV_DIR_VER);
    S.mgr_list = mlist;
    lv_obj_t* mfoot = box(mgr, lv_pct(100), 72);
    flex_row(mfoot);
    lv_obj_set_flex_align(mfoot, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(mfoot, "Scan for devices", scan_btn_cb, nullptr, C_GREEN, C_INK, 260, 48);

    /* --- discovery view --- */
    lv_obj_t* dv = box(panel, lv_pct(100), lv_pct(100));
    flex_col(dv);
    lv_obj_add_flag(dv, LV_OBJ_FLAG_HIDDEN);
    S.v_disco = dv;
    lv_obj_t* dh = box(dv, lv_pct(100), 54);
    flex_row(dh);
    lv_obj_set_style_pad_hor(dh, 16, 0);
    lv_obj_set_style_pad_column(dh, 10, 0);
    hairline_side(dh, LV_BORDER_SIDE_BOTTOM);
    make_btn(dh, LV_SYMBOL_LEFT, disco_back_cb, nullptr, C_SURF, C_HI, 44, 40);
    label(dh, "Discover", FONT_ROW, C_HI);
    lv_obj_t* dsp = box(dh, 0, 1); lv_obj_set_flex_grow(dsp, 1);
    make_btn(dh, "Rescan", rescan_cb, nullptr, C_SURF, C_HI, 96, 40);
    lv_obj_t* dlist = box(dv, lv_pct(100), 0);
    lv_obj_set_flex_grow(dlist, 1);
    flex_col(dlist);
    lv_obj_set_style_pad_hor(dlist, 8, 0);
    lv_obj_add_flag(dlist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(dlist, LV_DIR_VER);
    S.disco_list = dlist;

    /* --- PIN view --- */
    lv_obj_t* pv = box(panel, lv_pct(100), lv_pct(100));
    flex_col(pv);
    lv_obj_set_flex_align(pv, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(pv, 16, 0);
    lv_obj_add_flag(pv, LV_OBJ_FLAG_HIDDEN);
    S.v_pin = pv;
    S.pin_dev = label(pv, "Enter PIN", FONT_ROW, C_HI);
    label(pv, "Code shown on your radio (or 123456 if unset)", FONT_META, C_DIM);
    S.pin_disp = label(pv, "______", FONT_TITLE, C_GREEN);
    static const char* kDigit[10] = {"0","1","2","3","4","5","6","7","8","9"};
    /* standard 3-column keypad; bottom row: blank, 0, backspace */
    const int grid[4][3] = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}, {-1, 0, 10}};
    lv_obj_t* pad = box(pv, 300, 244);
    flex_col(pad);
    lv_obj_set_flex_align(pad, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(pad, 8, 0);
    for (int r = 0; r < 4; r++) {
        lv_obj_t* row = box(pad, 300, 52);
        flex_row(row);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 8, 0);
        for (int c = 0; c < 3; c++) {
            int v = grid[r][c];
            if (v == -1) { box(row, 92, 52); continue; }   /* blank cell */
            const char* lbl = (v == 10) ? LV_SYMBOL_BACKSPACE : kDigit[v];
            make_btn(row, lbl, key_cb, (void*)(intptr_t)v, C_SURF, C_HI, 92, 52);
        }
    }
    lv_obj_t* pf = box(pv, 304, 52);
    flex_row(pf);
    lv_obj_set_flex_align(pf, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pf, 10, 0);
    make_btn(pf, "Cancel", pin_cancel_cb, nullptr, C_SURF, C_HI, 140, 48);
    make_btn(pf, "Connect", pin_connect_cb, nullptr, C_GREEN, C_INK, 140, 48);

    radio_show(0);
    return panel;
}

void radio_refresh(void)
{
    app_snapshot_t s;
    app_state_snapshot(&s);

    /* The radio drives PIN entry: pop the keypad when it asks (CONN_ENTER_PIN),
     * and leave it once pairing advances. */
    if (s.state == CONN_ENTER_PIN && S.radio_view != 2) {
        S.pin_buf[0] = 0;
        update_pin_disp();
        if (S.pin_dev) {
            char t[40];
            snprintf(t, sizeof(t), "Pair %s", S.pin_name[0] ? S.pin_name : "radio");
            set_text(S.pin_dev, t);
        }
        radio_show(2);
    } else if (S.radio_view == 2 && s.state != CONN_ENTER_PIN) {
        radio_show(0);
    }

    /* manager status line */
    if (S.radio_view == 0 && S.mgr_status) {
        char line[64];
        if (s.state == CONN_READY)          snprintf(line, sizeof(line), "connected: %s", s.my_long);
        else if (s.state == CONN_NO_DEVICE) snprintf(line, sizeof(line), "no device");
        else                                snprintf(line, sizeof(line), "%s", s.stage);
        set_text(S.mgr_status, line);
    }
    /* discovery list refresh as new adverts arrive */
    if (S.radio_view == 1) {
        uint32_t gen = app_state_scan_gen();
        if (gen != S.last_scan_gen) { rebuild_discovery(); S.last_scan_gen = gen; }
    }
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
    lv_obj_add_flag(sb, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(sb, diag_toggle_cb, LV_EVENT_LONG_PRESSED, nullptr);

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
    S.panel[1] = make_chat_panel(content);
    S.panel[2] = make_radio_panel(content);
    S.detail   = make_detail_panel(content);   /* overlays the content area */

#if UI_DIAG_OVERLAY
    /* diagnostics strip pinned to the bottom — hidden until long-press summons it */
    lv_obj_t* diag = box(col, lv_pct(100), 22);
    bg(diag, C_CHROME);
    lv_obj_set_style_pad_hor(diag, 10, 0);
    hairline_side(diag, LV_BORDER_SIDE_TOP);
    lv_obj_add_flag(diag, LV_OBJ_FLAG_HIDDEN);
    S.diag_box = diag;
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

/* Physical-keyboard text routing. Called off the keyboard task; we take the
 * LVGL lock and drive the same paths the touch UI uses (do_send / the PIN
 * buffer), so input from either source behaves identically.
 *
 * STRING mode delivers one whole token per event: a single printable byte for
 * an ordinary key, or a *named* word for a special key ("backspace", "enter",
 * "space", ...). So we classify the token, not byte-by-byte — otherwise the
 * letters of a key name would be typed verbatim. Unknown named keys are dropped. */
enum kbd_action { KBD_NONE, KBD_TEXT, KBD_BACKSPACE, KBD_SUBMIT };

static kbd_action kbd_classify(const char* str, unsigned char len, char* text_out)
{
    text_out[0] = 0;
    if (len == 1) {
        char c = str[0];
        if (c == '\b' || c == 0x7f) return KBD_BACKSPACE;
        if (c == '\n' || c == '\r') return KBD_SUBMIT;
        if ((unsigned char)c >= 0x20) { text_out[0] = c; text_out[1] = 0; return KBD_TEXT; }
        return KBD_NONE;
    }
    /* Named special key — match case-insensitively. */
    char name[16];
    size_t n = len < sizeof(name) - 1 ? len : sizeof(name) - 1;
    for (size_t i = 0; i < n; i++) name[i] = (char)tolower((unsigned char)str[i]);
    name[n] = 0;

    if (!strcmp(name, "backspace") || !strcmp(name, "bksp") || !strcmp(name, "bs") ||
        !strcmp(name, "delete") || !strcmp(name, "del"))
        return KBD_BACKSPACE;
    if (!strcmp(name, "enter") || !strcmp(name, "return") || !strcmp(name, "ent"))
        return KBD_SUBMIT;
    if (!strcmp(name, "space")) { text_out[0] = ' '; text_out[1] = 0; return KBD_TEXT; }
    return KBD_NONE;   /* tab, arrows, fn keys, etc. — ignored */
}

void ui_kbd_feed(const char* str, unsigned char len, unsigned char modifier)
{
    if (!str || len == 0) return;
    /* Ctrl / Alt chords aren't text — ignore them (PIN and chat want plain keys). */
    if (modifier == 1 || modifier == 4 || modifier == 5) return;

    char text[2];
    kbd_action act = kbd_classify(str, len, text);
    if (act == KBD_NONE) return;

    if (!lvgl_port_lock(100)) return;   /* drop input rather than block the kbd task */

    if (S.active == 2 && S.radio_view == 2) {           /* PIN entry */
        size_t plen = strlen(S.pin_buf);
        if (act == KBD_BACKSPACE) {
            if (plen) { S.pin_buf[plen - 1] = 0; update_pin_disp(); }
        } else if (act == KBD_SUBMIT) {
            if (S.pin_buf[0]) ble_transport_submit_pin((uint32_t)atoi(S.pin_buf));
        } else if (act == KBD_TEXT && text[0] >= '0' && text[0] <= '9' && plen < 6) {
            S.pin_buf[plen] = text[0];
            S.pin_buf[plen + 1] = 0;
            update_pin_disp();
        }
    } else if (S.active == 1 && S.chat_input) {         /* chat composer */
        if (act == KBD_BACKSPACE) lv_textarea_delete_char(S.chat_input);
        else if (act == KBD_SUBMIT) do_send();
        else if (act == KBD_TEXT) lv_textarea_add_text(S.chat_input, text);
    }

    lvgl_port_unlock();
}
