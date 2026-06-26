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

/* ---- onboarding / device-picker commands (PRD §6.1) ---- */

/* Start a discovery scan: report Meshtastic adverts into app_state's scan list
 * (does not connect). */
void ble_transport_scan(void);

/* Connect to a specific device with the given numeric PIN, bond, and (on a
 * clean sync) persist it as the active device. Switches away from any current
 * connection. */
void ble_transport_connect(const uint8_t addr[6], uint32_t pin);

/* Forget a saved device: drop its bond and remove it from settings. If it is
 * the active device, disconnect. */
void ble_transport_forget(const uint8_t addr[6]);

#ifdef __cplusplus
}
#endif
