# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A green-field **Meshtastic BLE client for the M5Stack Tab5** (ESP32-P4 host +
onboard ESP32-C6 BLE controller over esp-hosted/SDIO). As of this writing the
directory contains **only `PRD.md`** — no code, no `CMakeLists.txt`, not yet a git
repo. The scaffold gets built out following the milestones in the PRD.

**`PRD.md` is the spec. Read it before doing anything.** It is not background — it
carries the requirements, the layered architecture, the UI design system, the
exact Meshtastic GATT contract, and (critically) a table of hardware facts that
cost real time to learn and must **not be re-derived**. This file only summarizes
the operational essentials; the PRD is authoritative.

This is a rebuild of `../Tab5-Meshtastic` (v1), which works end-to-end but
accreted bugs. v1 is kept as **reference, not a base to fork** — re-implement its
*guarantees*, not its code (PRD §R5).

## Build / flash

No build files exist yet. When scaffolding M0 (PRD §11), reuse v1's proven setup
verbatim — it is the known-good stack and the whole point of v2 is not to
re-discover it:

- **Target `esp32p4`**, SPIRAM on, custom `partitions.csv` (6M factory app).
- **Pinned deps** (`main/idf_component.yml`): `espressif/esp_hosted: 1.4.0` +
  `espressif/esp_wifi_remote: 0.8.5`. esp_hosted 1.4.0 bundles the **BT-capable
  C6 slave firmware** — that is what gives the radio-less P4 a BLE controller.
  Also pull in `m5_tab5_component` for board/expander/power bring-up.
- **NimBLE as host, controller REMOTE on the C6**: `CONFIG_BT_CONTROLLER_DISABLED=y`,
  `CONFIG_BT_NIMBLE_ENABLED=y`, `CONFIG_ESP_HOSTED_ENABLE_BT_NIMBLE=y`, central +
  observer roles only, security/SM (legacy + SC) enabled. The full block lives in
  `../Tab5-Meshtastic/sdkconfig.defaults` — copy it; the SDIO pin config comes
  from `../M5Tab5-UserDemo/platforms/tab5`.

```bash
idf.py set-target esp32p4
idf.py build
idf.py -p /dev/ttyACM0 flash monitor   # plug the Tab5 battery in FIRST
```

ESP-IDF v5.4.4 is at `~/esp/esp-idf`, auto-sourced, so `idf.py` is on PATH.

### Commit before every build

**Always `git commit` before running a build.** Because the only real test is an
on-hardware cold boot (serial resets the P4, USB browns out, and a bad board /
boot-path change can hard-hang the device), every build is a point where the
last known-good state must be recoverable. Commit first, then build, so any
regression is a `git diff` / `git revert` away rather than something to
reconstruct from memory.

This directory is **not yet a git repo** — initialize one (`git init`) as part of
the M0 scaffold so this discipline holds from the first build.

## Hardware facts that will burn time if ignored

These are load-bearing constraints, not tips (full table: PRD §4):

- **Battery must be plugged in to run/flash.** USB alone browns out the Tab5
  (blank screen, drops off USB).
- **Attaching serial (`/dev/ttyACM*`) resets the P4.** You cannot rely on live
  serial debugging in steady state — on-screen diagnostics (PRD §FR-5.1) are the
  primary observability surface. Port is plug-order dependent (ACM0 vs ACM1).
- **Cold boot from battery ≠ warm serial-reset boot.** A past regression
  (I²C keyboard) cold-boot-failed while warm dev boots hid it. Any change to board
  bring-up / power / boot path **must be validated on a real cold battery boot**.
  This is the M0 gate (20/20 cold boots) and is non-negotiable.
- **BLE sync is poll-driven, NOT notification-driven.** FromNum GATT notifications
  *never traverse* the esp-hosted tunnel. A ~600 ms periodic poll of FromRadio is
  the sync engine. Subscribe to FromNum anyway, but never depend on it.
- **FromRadio read completion callbacks occasionally never fire** over SDIO. Every
  read needs a **timeout (~1.5 s) → abandon → re-issue** recovery, or a naive
  single-in-flight guard wedges forever. Mandatory from day one.
- **Display is 1280×720 landscape via PPA hardware rotation** in display init.
  Runtime LVGL rotation corrupts the render — do not use it.
- Request **MTU 512** (negotiates to 255) and gate first reads on MTU. Don't
  `read_long` (Meshtastic pops a fresh packet per read). `want_config_id` and any
  retried ToRadio write must carry an **incrementing** id (the radio dedupes).

## Architecture (enforce, don't drift)

The one rule above all (PRD §5): **the BLE/protocol layer never touches LVGL, and
the UI never makes BLE calls except through a narrow command interface.** Four
layers, top to bottom:

1. **UI** (LVGL task only) — renders from an immutable `AppState` snapshot, emits
   Commands.
2. **AppState / Controller** — single source of truth behind one mutex; owns the
   one connection state machine.
3. **Meshtastic protocol layer** — pure nanopb encode/decode on byte buffers;
   **no BLE, no LVGL, no FreeRTOS** in its core, so it is unit-testable off-device.
4. **BLE transport** — NimBLE central: scan, bond, GATT, MTU, the poll/drain
   engine, read-timeout recovery, self-healing reconnect.

Thread boundaries are explicit: BLE→UI via snapshot, UI→BLE via a command queue,
nothing else. Keep the connection state machine in **one place**, not spread
across files (v1's main pain).

## Reference material (siblings under `../`)

- `Tab5-Meshtastic/` — working v1. `STATUS.md` (sync internals appendix),
  `README.md` (GATT + setup), `main/{main.cpp,ui.cpp,lcd_tools.cpp}` (proven
  backend, UI, PPA display init). `main.cpp` ~142–157 / ~499–561 is the
  poll/drain recovery contract to re-implement.
- `M5Tab5-UserDemo/` — known-good esp-hosted/SDIO + BSP baseline (has its own
  CLAUDE.md).
- `firmware/` — upstream Meshtastic firmware (server-side BLE reference:
  `src/nimble/NimbleBluetooth.cpp`, `src/mesh/PhoneAPI.{h,cpp}`).

### Test radio
M5Stack **Unit C6L**, BLE name `Meshtastic_0be4`, addr `XX:XX:XX:XX:XX:XX`,
FIXED_PIN `123456`. Its USB serial works (`meshtastic --port <dev> --info`) for
cross-checking what the radio reports against what the Tab5 renders.
