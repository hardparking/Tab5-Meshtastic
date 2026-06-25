/*
 * Tab5-Meshtastic v2 — AppState (single source of truth, PRD §5).
 *
 * All UI-visible mutable state lives here behind one mutex. The BLE/transport
 * layer (NimBLE host task + poll timer) PUBLISHES into it; the UI layer (LVGL
 * task) reads an immutable snapshot via app_state_snapshot(). Neither side
 * touches the other's objects — the snapshot is the only BLE→UI channel.
 *
 * M1 scope: connection state, my-node identity, and diagnostics counters. The
 * node DB + message log (their own ring buffers) arrive in M2/M4.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Connection state machine (PRD §FR-2.2). M1 auto-connects to a fixed radio, so
 * the device-picker states (SELECTING / ENTERING_PIN) are defined but unused
 * until M5. */
typedef enum {
    CONN_BOOT = 0,
    CONN_SCANNING,
    CONN_CONNECTING,
    CONN_PAIRING,       /* bonding / PIN exchange */
    CONN_SYNCING,       /* draining the config download */
    CONN_READY,
    CONN_RETRYING,
    CONN_DISCONNECTED,
    CONN_ERROR,
} conn_state_t;

/* Diagnostics counters mirrored on-screen (FR-5.1, NFR-7) because attaching
 * serial resets the P4. These are the transport's connection-scoped working
 * counters, published here for the UI. */
typedef struct {
    uint16_t mtu;          /* negotiated ATT MTU (23 until exchanged)        */
    uint16_t conn_itvl;    /* connection interval, ms (0 = unknown)          */
    uint32_t reads;        /* FromRadio reads that returned a packet         */
    uint32_t nodeinfo;     /* NodeInfo protobufs decoded                     */
    uint32_t decfail;      /* FromRadio protobufs that failed to decode      */
    uint32_t notifies;     /* FromNum notifications received (≈0 on hosted)  */
    uint32_t polls;        /* poll-timer ticks (the real drain mechanism)    */
    uint32_t read_tmos;    /* in-flight reads abandoned as lost (recovery)   */
    uint32_t wc_retries;   /* want_config re-sends                           */
    uint32_t sub_retries;  /* FromNum CCCD-write retries                     */
    bool     cccd_ok;      /* FromNum CCCD write confirmed                   */
    bool     wc_sent;      /* want_config write issued locally (rc==0)       */
    bool     wc_acked;     /* want_config ATT write-response from radio      */
    int      last_err;     /* last GATT error status seen                    */
} diag_t;

/* Immutable copy handed to the UI. */
typedef struct {
    conn_state_t state;
    char         stage[24];   /* short human label for the status chip      */
    uint32_t     my_num;      /* my node number (0 = unknown)               */
    char         my_long[40];
    char         my_short[8];
    uint32_t     node_count;  /* distinct nodes heard this sync             */
    diag_t       diag;
} app_snapshot_t;

/* ---- node DB (PRD §10) ---- */
#define APP_MAX_NODES 256

typedef struct {
    uint32_t num;
    char     long_name[40];
    char     short_name[8];
    float    snr;
    int      hops;           /* hops_away (0 = direct)            */
    bool     has_user;
    int64_t  last_heard_us;  /* esp_timer time at last update     */
} node_rec_t;

void app_state_init(void);

/* ---- publishers (called from the BLE/transport layer) ---- */
void app_state_set_conn(conn_state_t state, const char* stage);
void app_state_set_myinfo(uint32_t num, const char* long_name, const char* short_name);
void app_state_set_diag(const diag_t* diag);

/* Insert or update a node. Updates snr/last_heard silently; bumps the node
 * generation only on a MEANINGFUL change (new node, or changed name / hops /
 * has_user) so SNR jitter never triggers a full list repaint (FR-3.2). */
void app_state_upsert_node(uint32_t num, const char* long_name, const char* short_name,
                           float snr, int hops, bool has_user, int64_t now_us);
void app_state_clear_nodes(void);

/* ---- readers (called from the UI/LVGL task) ---- */
void app_state_snapshot(app_snapshot_t* out);

/* Generation counter: changes whenever the node list meaningfully changed. The
 * UI compares it to decide whether to rebuild rows. */
uint32_t app_state_nodes_gen(void);

/* Copy the node DB into out[0..max). Returns the number copied. */
uint32_t app_state_copy_nodes(node_rec_t* out, uint32_t max);

/* True for states where the radio link is up and healthy (drives chip color). */
bool conn_is_linked(conn_state_t state);

#ifdef __cplusplus
}
#endif
