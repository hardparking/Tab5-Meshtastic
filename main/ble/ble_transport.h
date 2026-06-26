/*
 * Tab5-Meshtastic v2 — BLE transport + sync engine (PRD §5 bottom layer).
 *
 * NimBLE central over the C6 (esp-hosted). Owns scan → bond → GATT discovery →
 * MTU → the poll/drain sync engine with read-timeout recovery and a self-
 * healing connection state machine. Decodes via mesh_proto and PUBLISHES into
 * app_state; it never touches LVGL.
 *
 * M1 connects to a fixed test radio (PRD §4 reference node). Runtime device
 * selection lands in M5.
 *
 * Ordering: the board must be powered (wlan_power) and esp_hosted_init() +
 * nimble_port_init() must have succeeded before ble_transport_start().
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the NimBLE host (security/MTU/bond store), then start the host task
 * and begin scanning on sync. Call once from app_main after nimble_port_init(). */
void ble_transport_start(void);

/* Narrow UI→BLE command: send a broadcast text message on the primary channel.
 * Safe to call from the LVGL task (NimBLE GATT writes are thread-safe). No-op if
 * not connected. */
void ble_transport_send_text(const char* text);

#ifdef __cplusplus
}
#endif
