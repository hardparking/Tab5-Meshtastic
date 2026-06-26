/*
 * Tab5-Meshtastic v2 — physical keyboard input (M5Stack Tab5 Keyboard, A164).
 *
 * The 70-key STM32 keyboard module hangs off its own I2C bus (addr 0x6D,
 * SDA=0 / SCL=1) — independent of the board's system I2C. We run it in STRING
 * mode (the module hands us decoded characters + a modifier, so we don't carry
 * a keymap) and poll it on a small task. Each decoded chunk is forwarded to the
 * UI layer via ui_kbd_feed(), which routes it to whichever surface is focused
 * (chat composer or PIN entry).
 *
 * Init is best-effort: if the keyboard isn't attached, kbd_start() logs and
 * returns — on-screen (touch) input still works.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/* Bring up the physical keyboard and start its input task. Call once after the
 * UI shell exists (ui_start()) — the input task drives UI objects. Non-fatal if
 * no keyboard is present. */
void kbd_start(void);

/* True once a physical keyboard has been detected and brought up. The UI uses
 * this to suppress the on-screen keyboard when hardware input is available. */
bool kbd_present(void);

#ifdef __cplusplus
}
#endif
