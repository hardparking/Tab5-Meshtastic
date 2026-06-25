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

void app_state_set_node_count(uint32_t count)
{
    lock();
    g.s.node_count = count;
    unlock();
}

void app_state_set_diag(const diag_t* diag)
{
    lock();
    g.s.diag = *diag;
    unlock();
}

void app_state_snapshot(app_snapshot_t* out)
{
    lock();
    *out = g.s;
    unlock();
}

bool conn_is_linked(conn_state_t state)
{
    return state == CONN_SYNCING || state == CONN_READY;
}
