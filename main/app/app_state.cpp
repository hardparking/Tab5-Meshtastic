/*
 * Tab5-Meshtastic v2 — AppState implementation.
 *
 * One mutex guards the whole struct. Publishers and the snapshot reader hold it
 * only for the duration of a struct copy, so neither task blocks the other for
 * long. The mutex is created before BLE/UI start, so there is no init race.
 */

#include "app_state.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace {

struct AppState {
    SemaphoreHandle_t mtx;
    app_snapshot_t    s;
    node_rec_t        nodes[APP_MAX_NODES];
    uint32_t          node_n;
    uint32_t          nodes_gen;
};

AppState g;

inline void lock() { xSemaphoreTake(g.mtx, portMAX_DELAY); }
inline void unlock() { xSemaphoreGive(g.mtx); }

void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

}  // namespace

void app_state_init(void)
{
    if (g.mtx) return;
    g.mtx = xSemaphoreCreateMutex();
    memset(&g.s, 0, sizeof(g.s));
    g.s.state    = CONN_BOOT;
    g.s.diag.mtu = 23;
    copy_str(g.s.stage, sizeof(g.s.stage), "BOOT");
}

void app_state_set_conn(conn_state_t state, const char* stage)
{
    lock();
    g.s.state = state;
    if (stage) copy_str(g.s.stage, sizeof(g.s.stage), stage);
    unlock();
}

void app_state_set_myinfo(uint32_t num, const char* long_name, const char* short_name)
{
    lock();
    g.s.my_num = num;
    if (long_name)  copy_str(g.s.my_long, sizeof(g.s.my_long), long_name);
    if (short_name) copy_str(g.s.my_short, sizeof(g.s.my_short), short_name);
    unlock();
}

void app_state_set_diag(const diag_t* diag)
{
    lock();
    g.s.diag = *diag;
    unlock();
}

void app_state_upsert_node(uint32_t num, const char* long_name, const char* short_name,
                           float snr, int hops, bool has_user, int64_t now_us)
{
    lock();
    node_rec_t* rec = nullptr;
    for (uint32_t i = 0; i < g.node_n; i++) {
        if (g.nodes[i].num == num) { rec = &g.nodes[i]; break; }
    }

    bool meaningful = false;
    if (!rec) {
        if (g.node_n >= APP_MAX_NODES) { unlock(); return; }  /* bounded (NFR-4) */
        rec = &g.nodes[g.node_n++];
        memset(rec, 0, sizeof(*rec));
        rec->num         = num;
        g.s.node_count   = g.node_n;
        meaningful       = true;   /* new node */
    }

    /* meaningful field changes (topology / identity), not SNR jitter */
    if (has_user) {
        if (!rec->has_user ||
            strncmp(rec->long_name, long_name ? long_name : "", sizeof(rec->long_name)) != 0 ||
            strncmp(rec->short_name, short_name ? short_name : "", sizeof(rec->short_name)) != 0) {
            meaningful = true;
        }
        copy_str(rec->long_name, sizeof(rec->long_name), long_name);
        copy_str(rec->short_name, sizeof(rec->short_name), short_name);
        rec->has_user = true;
    }
    if (rec->hops != hops) meaningful = true;
    rec->hops = hops;

    /* always-fresh fields (silent) */
    rec->snr           = snr;
    rec->last_heard_us = now_us;

    if (meaningful) g.nodes_gen++;
    unlock();
}

void app_state_clear_nodes(void)
{
    lock();
    g.node_n       = 0;
    g.s.node_count = 0;
    g.nodes_gen++;
    unlock();
}

void app_state_snapshot(app_snapshot_t* out)
{
    lock();
    *out = g.s;
    unlock();
}

uint32_t app_state_nodes_gen(void)
{
    lock();
    uint32_t gen = g.nodes_gen;
    unlock();
    return gen;
}

uint32_t app_state_copy_nodes(node_rec_t* out, uint32_t max)
{
    lock();
    uint32_t n = g.node_n < max ? g.node_n : max;
    memcpy(out, g.nodes, n * sizeof(node_rec_t));
    unlock();
    return n;
}

bool conn_is_linked(conn_state_t state)
{
    return state == CONN_SYNCING || state == CONN_READY;
}
