/*
 * Tab5-Meshtastic v2 — app shell (UI layer).
 *
 * Threading model (PRD §5): ALL LVGL object work happens on the LVGL task.
 * Later milestones' backend (BLE/protocol) will never touch LVGL directly — it
 * mutates a mutex-guarded AppState and the UI renders from an immutable
 * snapshot. This M0 shell is static chrome only: left rail (NODES / CHAT /
 * RADIO), status bar, and empty content panels. No live data, no BLE.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Build the app shell on the active screen. Call once after app_lcd_lvgl_init()
 * succeeds. Locks LVGL internally. */
void ui_start(void);

/* Feed decoded characters from the physical keyboard into the focused input
 * surface (chat composer or PIN entry, per the active tab). Bytes are raw ASCII:
 * '\b' = backspace, '\n'/'\r' = submit; printable bytes are inserted. `modifier`
 * follows the Tab5 keyboard convention (0 = none, 1 = Ctrl, 4 = Alt, 5 = both).
 * Safe to call from another task — takes the LVGL lock internally. */
void ui_kbd_feed(const char* str, unsigned char len, unsigned char modifier);

#ifdef __cplusplus
}
#endif
