/*
 * Tab5-Meshtastic v2 — Meshtastic protocol layer (pure, PRD §5/§10).
 *
 * Operates only on byte buffers: no BLE, no LVGL, no FreeRTOS. The transport
 * hands it raw FromRadio bytes and gets back a decoded, tagged mesh_event_t;
 * to send, it encodes a ToRadio into a caller-supplied buffer. This keeps the
 * nanopb dependency in one place and makes decoding unit-testable off-device
 * (the seed of the M2 protocol layer).
 *
 * Over BLE each characteristic carries a RAW protobuf — there is no 0x94c3
 * framing (that's serial/TCP only) — so one FromRadio read == one FromRadio
 * message, and we decode the buffer as-is.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MESH_EV_NONE = 0,
    MESH_EV_MY_INFO,          /* MyNodeInfo: my node number               */
    MESH_EV_NODE_INFO,        /* NodeInfo: a mesh node (name/snr/hops)    */
    MESH_EV_CONFIG_COMPLETE,  /* config download finished (echoes our id) */
    MESH_EV_CHANNEL,          /* a Channel record                         */
    MESH_EV_TEXT,             /* TEXT_MESSAGE_APP packet                  */
    MESH_EV_POSITION,         /* POSITION_APP packet (live position)      */
    MESH_EV_TELEMETRY,        /* TELEMETRY_APP packet (live metrics)      */
    MESH_EV_OTHER,            /* decoded fine, not a variant we act on    */
    MESH_EV_DECODE_FAIL,      /* protobuf decode failed                   */
} mesh_event_kind_t;

/* GPS position (PRD §6.3). lat/lon are 1e-7 degrees (multiply to get degrees). */
typedef struct {
    bool    has_loc;
    int32_t lat_i;
    int32_t lon_i;
    bool    has_alt;
    int32_t alt_m;        /* meters above MSL */
    uint32_t sats;        /* satellites in view */
} mesh_position_t;

/* Device telemetry (PRD §6.3). */
typedef struct {
    bool     has_batt;     uint32_t batt;        /* 0-100, >100 = powered */
    bool     has_volt;     float    volt;
    bool     has_chanutil; float    chan_util;   /* channel utilization %  */
    bool     has_airtx;    float    air_tx;      /* tx airtime %           */
    bool     has_uptime;   uint32_t uptime;      /* seconds                */
} mesh_metrics_t;

typedef struct {
    uint32_t        num;
    char            long_name[40];
    char            short_name[8];
    float           snr;
    int             hops;          /* hops_away (0 = direct)        */
    bool            hops_valid;    /* has_hops_away                 */
    bool            has_user;      /* false until a User sub-record arrives */
    bool            has_position;
    mesh_position_t position;
    bool            has_metrics;
    mesh_metrics_t  metrics;
} mesh_node_t;

typedef struct {
    uint32_t from;
    char     text[233];   /* Meshtastic Data payload max  */
} mesh_text_t;

typedef struct {
    mesh_event_kind_t kind;
    union {
        uint32_t        my_num;              /* MESH_EV_MY_INFO          */
        mesh_node_t     node;                /* MESH_EV_NODE_INFO        */
        uint32_t        config_complete_id;  /* MESH_EV_CONFIG_COMPLETE  */
        mesh_text_t     text;                /* MESH_EV_TEXT             */
        struct { uint32_t from; mesh_position_t pos; }     position;   /* MESH_EV_POSITION  */
        struct { uint32_t from; mesh_metrics_t metrics; }  telemetry;  /* MESH_EV_TELEMETRY */
        int             variant;             /* MESH_EV_OTHER (raw tag)  */
    } u;
} mesh_event_t;

/* Encode ToRadio{want_config_id = id} into buf. Returns bytes written, 0 on
 * failure. The id must INCREMENT across (re)sends — the radio dedupes
 * byte-identical consecutive ToRadio writes (PRD §4). */
size_t mesh_encode_want_config(uint32_t id, uint8_t* buf, size_t cap);

/* Decode one FromRadio protobuf. Always fills *ev (kind == MESH_EV_DECODE_FAIL
 * on failure). Returns true if decoding succeeded. */
bool mesh_decode_fromradio(const uint8_t* data, uint16_t len, mesh_event_t* ev);

#ifdef __cplusplus
}
#endif
