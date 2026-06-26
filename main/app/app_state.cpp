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

/* Find a node by num, or create a bare record. Returns nullptr only if the DB
 * is full. Sets *created. Caller holds the lock. */
node_rec_t* find_or_create_locked(uint32_t num, bool* created)
{
    for (uint32_t i = 0; i < g.node_n; i++)
        if (g.nodes[i].num == num) { *created = false; return &g.nodes[i]; }
    if (g.node_n >= APP_MAX_NODES) return nullptr;   /* bounded (NFR-4) */
    node_rec_t* rec = &g.nodes[g.node_n++];
    memset(rec, 0, sizeof(*rec));
    rec->num       = num;
    g.s.node_count = g.node_n;
    *created       = true;
    return rec;
}

void app_state_upsert_node(uint32_t num, const char* long_name, const char* short_name,
                           float snr, int hops, bool hops_valid, bool has_user, int64_t now_us)
{
    lock();
    bool created = false;
    node_rec_t* rec = find_or_create_locked(num, &created);
    if (!rec) { unlock(); return; }
    bool meaningful = created;   /* new node is a list change */

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
    if (rec->hops != hops || rec->hops_valid != hops_valid) meaningful = true;
    rec->hops       = hops;
    rec->hops_valid = hops_valid;

    /* always-fresh fields (silent) */
    rec->snr           = snr;
    rec->last_heard_us = now_us;

    if (meaningful) g.nodes_gen++;
    unlock();
}

void app_state_set_node_position(uint32_t num, const mesh_position_t* pos, int64_t now_us)
{
    lock();
    bool created = false;
    node_rec_t* rec = find_or_create_locked(num, &created);
    if (!rec) { unlock(); return; }
    rec->has_position  = true;
    rec->position      = *pos;
    rec->last_heard_us = now_us;
    if (created) g.nodes_gen++;   /* only the new-node case is list-visible */
    unlock();
}

void app_state_set_node_metrics(uint32_t num, const mesh_metrics_t* metrics, int64_t now_us)
{
    lock();
    bool created = false;
    node_rec_t* rec = find_or_create_locked(num, &created);
    if (!rec) { unlock(); return; }
    rec->has_metrics   = true;
    rec->metrics       = *metrics;
    rec->last_heard_us = now_us;
    if (created) g.nodes_gen++;
    unlock();
}

bool app_state_get_node(uint32_t num, node_rec_t* out)
{
    lock();
    bool found = false;
    for (uint32_t i = 0; i < g.node_n; i++) {
        if (g.nodes[i].num == num) { *out = g.nodes[i]; found = true; break; }
    }
    unlock();
    return found;
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
