/*
 * Tab5-Meshtastic v2 — BLE transport + sync engine.
 *
 * Re-implements the proven sync/recovery GUARANTEES from ../Tab5-Meshtastic
 * (not its coupling) against the clean layers:
 *   - poll-driven drain (FromNum notifications never traverse esp-hosted)
 *   - read-timeout recovery: a FromRadio read in flight > 1.5 s is abandoned
 *     and re-issued (the lost-callback wedge fix)
 *   - MTU-gated first read + incrementing want_config_id on every (re)issue
 *   - one self-healing connection state machine: stall → reconnect → unpair
 * Decoding goes through mesh_proto; all UI-visible state is published to
 * app_state. This file never includes LVGL.
 */

#include "ble_transport.h"

#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/ble_sm.h"

#include "app_state.h"
#include "mesh_proto.h"

extern "C" void ble_store_config_init(void);

static const char* TAG = "ble";

/* ---- Target radio: M5Stack Unit C6L (PRD §4 reference node) ----
 * BLE name "Meshtastic_0be4", addr XX:XX:XX:XX:XX:XX, fixed PIN 123456. M5
 * replaces this with a runtime-selected device. */
static const char*   kTargetName    = "Meshtastic";  /* prefix match */
static const uint8_t kTargetAddr[6] = {0xe6, 0x0b, 0x50, 0x81, 0x8c, 0x58};  /* little-endian */
static const uint32_t kFixedPin     = 123456;

/* ---- Meshtastic GATT UUIDs (128-bit, little-endian byte order) ---- */
/* service 6ba1b218-15a8-461f-9fa8-5dcae273eafd */
static const ble_uuid128_t kSvcUuid = BLE_UUID128_INIT(
    0xfd, 0xea, 0x73, 0xe2, 0xca, 0x5d, 0xa8, 0x9f,
    0x1f, 0x46, 0xa8, 0x15, 0x18, 0xb2, 0xa1, 0x6b);
/* ToRadio  f75c76d2-129e-4dad-a1dd-7866124401e7  (write) */
static const ble_uuid128_t kToRadioUuid = BLE_UUID128_INIT(
    0xe7, 0x01, 0x44, 0x12, 0x66, 0x78, 0xdd, 0xa1,
    0xad, 0x4d, 0x9e, 0x12, 0xd2, 0x76, 0x5c, 0xf7);
/* FromRadio 2c55e69e-4993-11ed-b878-0242ac120002 (read) */
static const ble_uuid128_t kFromRadioUuid = BLE_UUID128_INIT(
    0x02, 0x00, 0x12, 0xac, 0x42, 0x02, 0x78, 0xb8,
    0xed, 0x11, 0x93, 0x49, 0x9e, 0xe6, 0x55, 0x2c);
/* FromNum  ed9da18c-a800-4f66-a670-aa7547e34453 (read/notify) */
static const ble_uuid128_t kFromNumUuid = BLE_UUID128_INIT(
    0x53, 0x44, 0xe3, 0x47, 0x75, 0xaa, 0x70, 0xa6,
    0x66, 0x4f, 0x00, 0xa8, 0x8c, 0xa1, 0x9d, 0xed);

/* ---- connection-scoped handles ---- */
static struct {
    uint16_t conn_handle;
    uint16_t svc_start, svc_end;
    uint16_t toradio_handle;
    uint16_t fromradio_handle;
    uint16_t fromnum_handle;
    uint16_t fromnum_cccd;
} s_conn;

/* ---- sync engine state ---- */
static esp_timer_handle_t s_poll_timer    = nullptr;
static volatile bool      s_read_pending  = false;
static int64_t            s_read_started_us = 0;

static uint16_t s_mtu        = 23;     /* negotiated ATT MTU                  */
static bool     s_mtu_done   = false;  /* MTU exchange resolved               */
static bool     s_subscribed = false;  /* FromNum CCCD written                 */
static bool     s_drained    = false;  /* config download armed (once)        */
static bool     s_enc_done   = false;  /* link encrypted+authenticated        */
static bool     s_ready      = false;  /* CONFIG COMPLETE this connection      */
static uint32_t s_my_num     = 0;
static uint32_t s_wc_id      = 0x2a;   /* incrementing want_config id          */

/* diagnostics published to app_state */
static diag_t   s_d;

/* stall watchdog */
static bool       s_have_peer  = false;
static ble_addr_t s_peer_addr;
static int        s_sync_fails = 0;
static int64_t    s_progress_us = 0;
static uint32_t   s_prog_sig   = 0;
#define STALL_TIMEOUT_US   10000000    /* tear down if no progress for 10 s */
#define UNPAIR_AFTER_FAILS 2

static int  gap_event(struct ble_gap_event* event, void* arg);
static void start_scan(void);
static void try_drain(void);
static int  read_fromradio(void);

/* ---- publish helpers (the only writes to app_state) ---- */

static void publish_diag(void) { app_state_set_diag(&s_d); }

static void set_stage(conn_state_t state, const char* label)
{
    app_state_set_conn(state, label);
    publish_diag();
}

/* ============================ Scan ============================ */

static void start_scan(void)
{
    uint8_t own_addr_type;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "infer addr failed");
        return;
    }
    struct ble_gap_disc_params disc = {};
    disc.passive           = 0;   /* ACTIVE: pull scan-response for the name */
    disc.filter_duplicates = 1;
    ESP_LOGI(TAG, "active scan for the Meshtastic radio...");
    set_stage(CONN_SCANNING, "SCANNING");
    int rc = ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &disc, gap_event, NULL);
    if (rc != 0) ESP_LOGE(TAG, "ble_gap_disc rc=%d", rc);
}

static bool advert_is_target(const struct ble_gap_disc_desc* d)
{
    if (memcmp(d->addr.val, kTargetAddr, 6) == 0) return true;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) return false;
    return f.name && f.name_len &&
           strncmp((const char*)f.name, kTargetName, strlen(kTargetName)) == 0;
}

/* ====================== ToRadio writes ======================= */

static int wantcfg_write_cb(uint16_t, const struct ble_gatt_error* error,
                            struct ble_gatt_attr*, void*)
{
    if (error->status == 0) {
        if (!s_d.wc_acked) ESP_LOGI(TAG, "want_config ACKed by radio");
        s_d.wc_acked = true;
    } else {
        ESP_LOGW(TAG, "want_config write status=%d (will re-send)", error->status);
    }
    publish_diag();
    return 0;
}

/* The radio dedupes byte-identical consecutive ToRadio writes, so every
 * (re)send must carry a fresh, incrementing id (PRD §4). */
static void send_want_config(void)
{
    uint8_t buf[32];
    size_t n = mesh_encode_want_config(s_wc_id++, buf, sizeof(buf));
    if (n == 0) { ESP_LOGE(TAG, "want_config encode failed"); return; }
    int rc = ble_gattc_write_flat(s_conn.conn_handle, s_conn.toradio_handle,
                                  buf, n, wantcfg_write_cb, NULL);
    s_d.wc_sent = (rc == 0);
    ESP_LOGI(TAG, "want_config write (%u bytes) rc=%d", (unsigned)n, rc);
    publish_diag();
}

/* ====================== FromNum subscribe ===================== */

static int subscribe_cb(uint16_t, const struct ble_gatt_error* error,
                        struct ble_gatt_attr*, void*)
{
    if (error->status == 0) {
        s_d.cccd_ok = true;
        ESP_LOGI(TAG, "FromNum CCCD confirmed (notifications enabled)");
    } else {
        ESP_LOGW(TAG, "FromNum CCCD status=%d (will retry)", error->status);
    }
    publish_diag();
    return 0;
}

static void write_cccd_subscribe(void)
{
    uint8_t cccd_notify[2] = {0x01, 0x00};
    int rc = ble_gattc_write_flat(s_conn.conn_handle, s_conn.fromnum_cccd,
                                  cccd_notify, sizeof(cccd_notify), subscribe_cb, NULL);
    ESP_LOGI(TAG, "subscribe FromNum CCCD(0x%04x) rc=%d", s_conn.fromnum_cccd, rc);
}

static void subscribe_and_request(void)
{
    write_cccd_subscribe();
    s_subscribed = true;
    try_drain();
}

/* Kick the config download exactly once, gated on BOTH a resolved MTU and a
 * FromNum subscribe — an early read at MTU 23 truncates NodeInfo and stalls
 * sync at one node (PRD §4). Whichever of MTU/subscribe finishes last wins. */
static void try_drain(void)
{
    if (!s_mtu_done || !s_subscribed || s_drained) return;
    s_drained = true;
    ESP_LOGI(TAG, "MTU=%u settled + subscribed -> config download", (unsigned)s_mtu);
    send_want_config();
    read_fromradio();
}

/* ====================== GATT discovery ======================= */

static int dsc_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                  uint16_t, const struct ble_gatt_dsc* dsc, void*)
{
    if (error->status == 0 && dsc &&
        ble_uuid_u16(&dsc->uuid.u) == BLE_GATT_DSC_CLT_CFG_UUID16) {
        s_conn.fromnum_cccd = dsc->handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (s_conn.fromnum_cccd) subscribe_and_request();
        else ESP_LOGE(TAG, "no CCCD found on FromNum");
    }
    return 0;
}

static int chr_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                  const struct ble_gatt_chr* chr, void*)
{
    if (error->status == 0 && chr) {
        if (ble_uuid_cmp(&chr->uuid.u, &kToRadioUuid.u) == 0)
            s_conn.toradio_handle = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &kFromRadioUuid.u) == 0)
            s_conn.fromradio_handle = chr->val_handle;
        else if (ble_uuid_cmp(&chr->uuid.u, &kFromNumUuid.u) == 0)
            s_conn.fromnum_handle = chr->val_handle;
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
        ESP_LOGI(TAG, "chars: ToRadio=0x%04x FromRadio=0x%04x FromNum=0x%04x",
                 s_conn.toradio_handle, s_conn.fromradio_handle, s_conn.fromnum_handle);
        if (!s_conn.toradio_handle || !s_conn.fromradio_handle || !s_conn.fromnum_handle) {
            ESP_LOGE(TAG, "missing a characteristic; aborting");
            return 0;
        }
        ble_gattc_disc_all_dscs(conn_handle, s_conn.fromnum_handle, s_conn.svc_end, dsc_cb, NULL);
    }
    return 0;
}

static int svc_cb(uint16_t conn_handle, const struct ble_gatt_error* error,
                  const struct ble_gatt_svc* svc, void*)
{
    if (error->status == 0 && svc) {
        s_conn.svc_start = svc->start_handle;
        s_conn.svc_end   = svc->end_handle;
    } else if (error->status == BLE_HS_EDONE) {
        if (!s_conn.svc_start) { ESP_LOGE(TAG, "Meshtastic service not found"); return 0; }
        ESP_LOGI(TAG, "svc [0x%04x..0x%04x]; discovering chars", s_conn.svc_start, s_conn.svc_end);
        ble_gattc_disc_all_chrs(conn_handle, s_conn.svc_start, s_conn.svc_end, chr_cb, NULL);
    }
    return 0;
}

/* ====================== FromRadio reads ====================== */

static void handle_event(const mesh_event_t* ev, uint16_t len)
{
    switch (ev->kind) {
    case MESH_EV_DECODE_FAIL:
        ESP_LOGW(TAG, "FromRadio decode failed (%u bytes)", len);
        s_d.decfail++;
        publish_diag();
        break;
    case MESH_EV_MY_INFO:
        s_my_num = ev->u.my_num;
        ESP_LOGI(TAG, "MyNodeInfo: my node num = 0x%08lx", (unsigned long)s_my_num);
        app_state_set_myinfo(s_my_num, NULL, NULL);
        break;
    case MESH_EV_NODE_INFO: {
        const mesh_node_t* n = &ev->u.node;
        ESP_LOGI(TAG, "NodeInfo: 0x%08lx %s snr=%.1f hops=%d",
                 (unsigned long)n->num, n->has_user ? n->long_name : "(no user)",
                 n->snr, n->hops);
        app_state_upsert_node(n->num, n->long_name, n->short_name,
                              n->snr, n->hops, n->has_user, esp_timer_get_time());
        if (n->num == s_my_num && n->has_user)
            app_state_set_myinfo(n->num, n->long_name, n->short_name);
        s_d.nodeinfo++;
        publish_diag();
        break;
    }
    case MESH_EV_CONFIG_COMPLETE:
        ESP_LOGW(TAG, "*** CONFIG COMPLETE (id=0x%lx) — node DB synced ***",
                 (unsigned long)ev->u.config_complete_id);
        s_ready      = true;
        s_sync_fails = 0;
        set_stage(CONN_READY, "READY");
        break;
    case MESH_EV_TEXT:
        ESP_LOGW(TAG, "TEXT from 0x%08lx: \"%s\"",
                 (unsigned long)ev->u.text.from, ev->u.text.text);
        /* M4 routes this into the chat message log. */
        break;
    case MESH_EV_CHANNEL:
    case MESH_EV_OTHER:
    default:
        break;
    }
}

static int read_cb(uint16_t, const struct ble_gatt_error* error,
                   struct ble_gatt_attr* attr, void*)
{
    s_read_pending = false;
    if (error->status != 0) {
        if (error->status != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "FromRadio read status=%d", error->status);
            s_d.last_err = error->status;
            publish_diag();
        }
        return 0;
    }
    uint16_t len = attr->om ? OS_MBUF_PKTLEN(attr->om) : 0;
    if (len == 0) return 0;   /* queue empty; poll/next-notify resumes draining */

    uint8_t buf[512];
    uint16_t out = 0;
    ble_hs_mbuf_to_flat(attr->om, buf, sizeof(buf), &out);
    s_d.reads++;
    publish_diag();

    mesh_event_t ev;
    mesh_decode_fromradio(buf, out, &ev);
    handle_event(&ev, out);

    read_fromradio();   /* keep reading until empty */
    return 0;
}

static int read_fromradio(void)
{
    /* Never read before the gated drain has armed (want_config sent at a settled
     * MTU); an early read at MTU 23 truncates the first packets. */
    if (!s_drained || s_read_pending || s_conn.fromradio_handle == 0) return 0;
    s_read_pending    = true;
    s_read_started_us = esp_timer_get_time();
    int rc = ble_gattc_read(s_conn.conn_handle, s_conn.fromradio_handle, read_cb, NULL);
    if (rc != 0) { s_read_pending = false; s_d.last_err = rc; }
    return rc;
}

/* ================ poll timer: drain + recovery =============== */

static void poll_cb(void*)
{
    s_d.polls++;

    /* Read-timeout recovery: a FromRadio read whose completion callback never
     * arrived (lost over the esp-hosted/SDIO HCI tunnel) would otherwise leave
     * s_read_pending stuck forever, wedging the whole drain. Abandon it. */
    if (s_read_pending && esp_timer_get_time() - s_read_started_us > 1500000) {
        s_read_pending = false;
        s_d.read_tmos++;
    }

    /* The CCCD subscribe write can be lost like a read; retry a few times. (Even
     * when confirmed, notifications never arrive over esp-hosted — the poll below
     * is the real drain; this is cheap insurance.) */
    if (s_drained && !s_d.cccd_ok && s_d.sub_retries < 5 && (s_d.polls % 3) == 0) {
        s_d.sub_retries++;
        ESP_LOGW(TAG, "FromNum unconfirmed; retry subscribe #%lu", (unsigned long)s_d.sub_retries);
        write_cccd_subscribe();
    }

    /* Self-healing want_config: the write returns rc==0 locally but can be
     * silently dropped on the link, leaving the radio with nothing queued (every
     * read empty — the "stuck SYNCING, only own node" bug). If nothing has
     * arrived yet, re-issue (idempotent; restarts the config stream). Stop the
     * moment any packet arrives. */
    if (s_drained && s_d.reads == 0 && s_d.wc_retries < 15 && (s_d.polls % 3) == 0) {
        s_d.wc_retries++;
        ESP_LOGW(TAG, "no config stream yet; re-send want_config #%lu", (unsigned long)s_d.wc_retries);
        send_want_config();
    }

    /* Stall watchdog: progress == any milestone/packet advancing. No progress
     * for STALL_TIMEOUT_US means the link is wedged — tear down and reconnect;
     * after repeated stalls drop the bond to force a fresh pairing. */
    if (!s_ready) {
        uint32_t sig = (uint32_t)s_mtu_done + s_enc_done + s_d.cccd_ok + s_drained +
                       s_d.reads + s_d.nodeinfo;
        int64_t now = esp_timer_get_time();
        if (sig != s_prog_sig) { s_prog_sig = sig; s_progress_us = now; }
        else if (now - s_progress_us > STALL_TIMEOUT_US) {
            s_progress_us = now;   /* debounce until DISCONNECT lands */
            s_sync_fails++;
            ESP_LOGW(TAG, "sync STALLED (fails=%d) — reconnecting", s_sync_fails);
            if (s_sync_fails >= UNPAIR_AFTER_FAILS && s_have_peer) {
                ESP_LOGW(TAG, "repeated stalls — dropping bond to force fresh pairing");
                ble_gap_unpair(&s_peer_addr);
                s_sync_fails = 0;
            }
            set_stage(CONN_RETRYING, "RETRYING");
            ble_gap_terminate(s_conn.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            return;
        }
    }
    publish_diag();
    read_fromradio();
}

/* ======================= GATT/MTU callbacks ================== */

static int mtu_cb(uint16_t, const struct ble_gatt_error* error, uint16_t mtu, void*)
{
    if (error->status == 0) s_mtu = mtu;
    s_mtu_done = true;
    s_d.mtu    = s_mtu;
    ESP_LOGI(TAG, "MTU exchange done status=%d mtu=%u", error->status, (unsigned)s_mtu);
    publish_diag();
    try_drain();
    return 0;
}

/* ======================= GAP events ========================== */

static void reset_conn_state(void)
{
    s_mtu = 23; s_mtu_done = false; s_subscribed = false; s_drained = false;
    s_enc_done = false; s_ready = false; s_my_num = 0;
    s_read_pending = false;
    s_progress_us = esp_timer_get_time(); s_prog_sig = 0;
    memset(&s_d, 0, sizeof(s_d));
    s_d.mtu = 23;
    app_state_clear_nodes();   /* fresh sync repopulates the DB */
}

static int gap_event(struct ble_gap_event* event, void* arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        if (!advert_is_target(&event->disc)) return 0;
        ESP_LOGI(TAG, ">>> found radio, rssi=%d — connecting", event->disc.rssi);
        ble_gap_disc_cancel();
        set_stage(CONN_CONNECTING, "CONNECTING");
        uint8_t own;
        ble_hs_id_infer_auto(0, &own);
        int rc = ble_gap_connect(own, &event->disc.addr, 30000, NULL, gap_event, NULL);
        if (rc != 0) { ESP_LOGE(TAG, "connect rc=%d; rescanning", rc); start_scan(); }
        return 0;
    }

    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGE(TAG, "connect failed status=%d; rescanning", event->connect.status);
            start_scan();
            return 0;
        }
        s_conn.conn_handle = event->connect.conn_handle;
        reset_conn_state();
        {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(s_conn.conn_handle, &d) == 0) {
                s_peer_addr = d.peer_id_addr;
                s_have_peer = true;
            }
        }
        set_stage(CONN_PAIRING, "PAIRING");
        ESP_LOGI(TAG, "connected (0x%04x); MTU + pairing", s_conn.conn_handle);
        ble_gattc_exchange_mtu(s_conn.conn_handle, mtu_cb, NULL);
        ble_gap_security_initiate(s_conn.conn_handle);
        if (!s_poll_timer) {
            esp_timer_create_args_t a = {};
            a.callback = poll_cb;
            a.name     = "frpoll";
            esp_timer_create(&a, &s_poll_timer);
        }
        esp_timer_start_periodic(s_poll_timer, 600000);   /* 600 ms drain */
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        if (event->passkey.params.action == BLE_SM_IOACT_INPUT) {
            struct ble_sm_io io = {};
            io.action  = BLE_SM_IOACT_INPUT;
            io.passkey = kFixedPin;
            int rc = ble_sm_inject_io(event->passkey.conn_handle, &io);
            ESP_LOGI(TAG, "injected fixed PIN rc=%d", rc);
        }
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "enc change status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            s_enc_done = true;
            set_stage(CONN_SYNCING, "SYNCING");
            /* Ask for a fast interval so the bulk node download isn't paced by a
             * slow default; the radio may clamp (result via CONN_UPDATE). */
            struct ble_gap_upd_params up = {};
            up.itvl_min = 12;   /* 15 ms */
            up.itvl_max = 24;   /* 30 ms */
            up.latency  = 0;
            up.supervision_timeout = 400;   /* 4 s */
            ble_gap_update_params(s_conn.conn_handle, &up);
            memset(&s_conn.svc_start, 0, sizeof(s_conn) - sizeof(s_conn.conn_handle));
            ble_gattc_disc_svc_by_uuid(s_conn.conn_handle, &kSvcUuid.u, svc_cb, NULL);
        } else {
            /* Failed encryption is the stale/mismatched-bond signature; drop the
             * bond now so the next attempt pairs fresh. */
            ESP_LOGW(TAG, "encryption FAILED — dropping stale bond + reconnecting");
            if (s_have_peer) ble_gap_unpair(&s_peer_addr);
            ble_gap_terminate(s_conn.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (event->notify_rx.attr_handle == s_conn.fromnum_handle) {
            s_d.notifies++;
            publish_diag();
            read_fromradio();
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        s_mtu      = event->mtu.value;
        s_mtu_done = true;
        s_d.mtu    = s_mtu;
        ESP_LOGI(TAG, "MTU now %u", (unsigned)s_mtu);
        publish_diag();
        try_drain();
        return 0;

    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc d;
        if (ble_gap_conn_find(s_conn.conn_handle, &d) == 0) {
            s_d.conn_itvl = (uint16_t)(d.conn_itvl * 5 / 4);   /* 1.25 ms units -> ms */
            publish_diag();
        }
        return 0;
    }

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected reason=%d; rescanning", event->disconnect.reason);
        if (s_poll_timer) esp_timer_stop(s_poll_timer);
        s_read_pending = false;
        s_drained      = false;
        set_stage(CONN_DISCONNECTED, "DISCONNECTED");
        memset(&s_conn, 0, sizeof(s_conn));
        start_scan();
        return 0;

    default:
        return 0;
    }
}

/* ====================== NimBLE host =========================== */

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0) { ESP_LOGE(TAG, "no BLE addr"); return; }
    ESP_LOGI(TAG, "BLE host synced; MTU pref=%d", ble_att_preferred_mtu());
    start_scan();
}

static void on_reset(int reason) { ESP_LOGW(TAG, "BLE host reset; reason=%d", reason); }

static void host_task(void*)
{
    ESP_LOGI(TAG, "NimBLE host task started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void ble_transport_start(void)
{
    s_d.mtu = 23;

    /* central that can input a passkey; bond with MITM + SC */
    ble_hs_cfg.sync_cb           = on_sync;
    ble_hs_cfg.reset_cb          = on_reset;
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_KEYBOARD_ONLY;
    ble_hs_cfg.sm_bonding        = 1;
    ble_hs_cfg.sm_mitm           = 1;
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_att_set_preferred_mtu(512);   /* Meshtastic packets can be large */
    ble_store_config_init();          /* persist bonds in NVS */

    nimble_port_freertos_init(host_task);
    ESP_LOGI(TAG, "transport started; waiting for BLE sync...");
}
