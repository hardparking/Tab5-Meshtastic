/*
 * Tab5-Meshtastic v2 — persistent settings (PRD §6.1 FR-1.4/1.5).
 *
 * NVS-backed storage for the selected radio: the active device (auto-connect
 * target) plus the list of saved devices. The NimBLE bond store keeps the
 * pairing keys separately; this keeps the address / PIN / friendly name so we
 * can reconnect (and re-bond) without re-entering anything.
 *
 * Plain C API over its own NVS namespace; safe to call from any task (NVS is
 * internally locked). Writes only happen on actual change to spare flash.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_state.h"   /* saved_device_t */

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_MAX_SAVED 8

void settings_init(void);

/* Active (auto-connect) device. Returns false if none is set. */
bool settings_get_active(saved_device_t* out);

/* Set the active device and upsert it into the saved list. No-op write if it
 * already matches what's stored. */
void settings_set_active(const saved_device_t* dev);

/* Copy the saved devices into out[0..max). Returns the count. */
uint32_t settings_get_saved(saved_device_t* out, uint32_t max);

/* True if this address is in the saved list. If pin_out != NULL and found, the
 * saved PIN is copied there (>= 8 bytes). */
bool settings_is_saved(const uint8_t addr[6], char* pin_out);

/* Remove a device from the saved list; clears the active device if it matches. */
void settings_forget(const uint8_t addr[6]);

#ifdef __cplusplus
}
#endif
