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

namespace {

void copy_str(char* dst, size_t cap, const char* src)
{
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
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
        ev->kind          = MESH_EV_NODE_INFO;
        ev->u.node.num    = ni->num;
        ev->u.node.snr    = ni->snr;
        ev->u.node.hops   = (int)ni->hops_away;
        ev->u.node.has_user = ni->has_user;
        if (ni->has_user) {
            copy_str(ev->u.node.long_name, sizeof(ev->u.node.long_name), ni->user.long_name);
            copy_str(ev->u.node.short_name, sizeof(ev->u.node.short_name), ni->user.short_name);
        }
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
        if (mp->which_payload_variant == meshtastic_MeshPacket_decoded_tag &&
            mp->decoded.portnum == meshtastic_PortNum_TEXT_MESSAGE_APP) {
            ev->kind         = MESH_EV_TEXT;
            ev->u.text.from  = mp->from;
            size_t n = mp->decoded.payload.size;
            if (n > sizeof(ev->u.text.text) - 1) n = sizeof(ev->u.text.text) - 1;
            memcpy(ev->u.text.text, mp->decoded.payload.bytes, n);
            ev->u.text.text[n] = 0;
        } else {
            ev->kind      = MESH_EV_OTHER;
            ev->u.variant = (int)fr.which_payload_variant;
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
