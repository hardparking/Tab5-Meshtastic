/*
 * Tab5-Meshtastic v2 — Meshtastic protocol layer implementation.
 *
 * Pure nanopb encode/decode. No BLE, no LVGL, no FreeRTOS includes — keep it
 * that way so this file can be compiled and tested off-device.
 */

#include "mesh_proto.h"

#include <string.h>

#include "pb_decode.h"
#include "pb_encode.h"
#include "meshtastic/mesh.pb.h"
#include "meshtastic/portnums.pb.h"
#include "meshtastic/telemetry.pb.h"

namespace {

void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

void map_position(const meshtastic_Position* p, mesh_position_t* out)
{
    out->has_loc = p->has_latitude_i && p->has_longitude_i;
    out->lat_i   = p->latitude_i;
    out->lon_i   = p->longitude_i;
    out->has_alt = p->has_altitude;
    out->alt_m   = p->altitude;
    out->sats    = p->sats_in_view;
}

void map_metrics(const meshtastic_DeviceMetrics* m, mesh_metrics_t* out)
{
    out->has_batt     = m->has_battery_level;        out->batt      = m->battery_level;
    out->has_volt     = m->has_voltage;              out->volt      = m->voltage;
    out->has_chanutil = m->has_channel_utilization;  out->chan_util = m->channel_utilization;
    out->has_airtx    = m->has_air_util_tx;          out->air_tx    = m->air_util_tx;
    out->has_uptime   = m->has_uptime_seconds;       out->uptime    = m->uptime_seconds;
}

}  // namespace

size_t mesh_encode_want_config(uint32_t id, uint8_t* buf, size_t cap)
{
    meshtastic_ToRadio t   = meshtastic_ToRadio_init_zero;
    t.which_payload_variant = meshtastic_ToRadio_want_config_id_tag;
    t.want_config_id        = id;

    pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &t)) return 0;
    return os.bytes_written;
}

size_t mesh_encode_text(const char* text, uint8_t* buf, size_t cap)
{
    meshtastic_ToRadio t    = meshtastic_ToRadio_init_zero;
    t.which_payload_variant = meshtastic_ToRadio_packet_tag;
    meshtastic_MeshPacket* p = &t.packet;
    p->to                     = 0xffffffff;   /* broadcast */
    p->which_payload_variant  = meshtastic_MeshPacket_decoded_tag;
    p->decoded.portnum        = meshtastic_PortNum_TEXT_MESSAGE_APP;
    size_t n = strlen(text);
    if (n > sizeof(p->decoded.payload.bytes)) n = sizeof(p->decoded.payload.bytes);
    memcpy(p->decoded.payload.bytes, text, n);
    p->decoded.payload.size = n;

    pb_ostream_t os = pb_ostream_from_buffer(buf, cap);
    if (!pb_encode(&os, meshtastic_ToRadio_fields, &t)) return 0;
    return os.bytes_written;
}

bool mesh_decode_fromradio(const uint8_t* data, uint16_t len, mesh_event_t* ev)
{
    memset(ev, 0, sizeof(*ev));

    meshtastic_FromRadio fr = meshtastic_FromRadio_init_zero;
    pb_istream_t is = pb_istream_from_buffer(data, len);
    if (!pb_decode(&is, meshtastic_FromRadio_fields, &fr)) {
        ev->kind = MESH_EV_DECODE_FAIL;
        return false;
    }

    switch (fr.which_payload_variant) {
    case meshtastic_FromRadio_my_info_tag:
        ev->kind        = MESH_EV_MY_INFO;
        ev->u.my_num    = fr.my_info.my_node_num;
        break;

    case meshtastic_FromRadio_node_info_tag: {
        const meshtastic_NodeInfo* ni = &fr.node_info;
        ev->kind            = MESH_EV_NODE_INFO;
        ev->u.node.num      = ni->num;
        ev->u.node.snr      = ni->snr;
        ev->u.node.hops     = (int)ni->hops_away;
        ev->u.node.hops_valid = ni->has_hops_away;
        ev->u.node.has_user = ni->has_user;
        if (ni->has_user) {
            copy_str(ev->u.node.long_name, sizeof(ev->u.node.long_name), ni->user.long_name);
            copy_str(ev->u.node.short_name, sizeof(ev->u.node.short_name), ni->user.short_name);
        }
        ev->u.node.has_position = ni->has_position;
        if (ni->has_position) map_position(&ni->position, &ev->u.node.position);
        ev->u.node.has_metrics = ni->has_device_metrics;
        if (ni->has_device_metrics) map_metrics(&ni->device_metrics, &ev->u.node.metrics);
        break;
    }

    case meshtastic_FromRadio_config_complete_id_tag:
        ev->kind                  = MESH_EV_CONFIG_COMPLETE;
        ev->u.config_complete_id  = fr.config_complete_id;
        break;

    case meshtastic_FromRadio_channel_tag:
        ev->kind = MESH_EV_CHANNEL;
        break;

    case meshtastic_FromRadio_packet_tag: {
        const meshtastic_MeshPacket* mp = &fr.packet;
        if (mp->which_payload_variant != meshtastic_MeshPacket_decoded_tag) {
            ev->kind      = MESH_EV_OTHER;   /* encrypted — can't read it */
            ev->u.variant = (int)fr.which_payload_variant;
            break;
        }
        const meshtastic_Data* d = &mp->decoded;
        if (d->portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
            ev->kind         = MESH_EV_TEXT;
            ev->u.text.from  = mp->from;
            size_t n = d->payload.size;
            if (n > sizeof(ev->u.text.text) - 1) n = sizeof(ev->u.text.text) - 1;
            memcpy(ev->u.text.text, d->payload.bytes, n);
            ev->u.text.text[n] = 0;
        } else if (d->portnum == meshtastic_PortNum_POSITION_APP) {
            /* payload is an inner Position protobuf */
            meshtastic_Position p = meshtastic_Position_init_zero;
            pb_istream_t ps = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
            if (pb_decode(&ps, meshtastic_Position_fields, &p)) {
                ev->kind             = MESH_EV_POSITION;
                ev->u.position.from  = mp->from;
                map_position(&p, &ev->u.position.pos);
            } else {
                ev->kind = MESH_EV_OTHER;
            }
        } else if (d->portnum == meshtastic_PortNum_TELEMETRY_APP) {
            /* payload is an inner Telemetry protobuf; we want device_metrics */
            meshtastic_Telemetry t = meshtastic_Telemetry_init_zero;
            pb_istream_t ts = pb_istream_from_buffer(d->payload.bytes, d->payload.size);
            if (pb_decode(&ts, meshtastic_Telemetry_fields, &t) &&
                t.which_variant == meshtastic_Telemetry_device_metrics_tag) {
                ev->kind              = MESH_EV_TELEMETRY;
                ev->u.telemetry.from  = mp->from;
                map_metrics(&t.variant.device_metrics, &ev->u.telemetry.metrics);
            } else {
                ev->kind = MESH_EV_OTHER;
            }
        } else {
            ev->kind      = MESH_EV_OTHER;
            ev->u.variant = (int)d->portnum;
        }
        break;
    }

    default:
        ev->kind      = MESH_EV_OTHER;
        ev->u.variant = (int)fr.which_payload_variant;
        break;
    }
    return true;
}
