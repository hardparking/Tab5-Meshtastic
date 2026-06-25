# Tab5-Meshtastic v2 — Product Requirements Document

> A Meshtastic BLE client for the M5Stack Tab5 (ESP32-P4 + ESP32-C6). Green-field
> rebuild. The previous attempt (`../Tab5-Meshtastic`) proved the path works end
> to end but accreted bugs across many iterations; this PRD specifies a clean
> rebuild that treats **stability, clean architecture, and UI responsiveness** as
> first-class requirements, with the old repo kept only as reference.

Status: **Draft for build** · Owner: Adam · Date: 2026-06-24

---

## 1. Vision & one-line goal

Turn the M5Stack Tab5 into a reliable, good-looking, **always-on Meshtastic
control surface**: pair to a Meshtastic radio over BLE, see the mesh, read its
nodes in detail (position + telemetry), and exchange text — and never have to
recompile to point it at a different radio.

The bar for "done" is not feature count. It is: **boots cleanly from cold
battery every time, connects without babysitting, and the UI never janks.**

---

## 2. Goals (v1)

1. **Onboarding / device picker** — scan for Meshtastic devices, pick one, enter
   its PIN, persist the choice. No hardcoded address or PIN. This is the headline
   v1 feature.
2. **Node detail + telemetry** — tap any node to open a detail view with position
   (GPS), battery/voltage, last-heard, hops, SNR, and device metrics. The second
   headline v1 feature.
3. **Solid broadcast text chat** — table-stakes underneath the above: send and
   receive `TEXT_MESSAGE_APP` on the primary channel, reliably, with an on-screen
   keyboard.
4. **A connection that heals itself** — the user should never see a permanent
   "syncing" hang. Connection issues recover automatically or surface a clear,
   actionable state.
5. **Rock-solid boot** — cold boot from battery succeeds every time, no brownout
   / render-corruption / hang regressions.

## 3. Non-goals (explicitly deferred to v2+)

- Channels beyond the primary channel; channel tabs.
- Direct messages to a specific node.
- Message delivery acks (✓✓) and per-message timestamps with a synced wall clock.
- Physical (I²C) hardware keyboard. **Deferred and gated** — see §11.
- Sending position/telemetry *from* the Tab5 (we are a viewer/chat client, not a
  full node).
- Map view, traceroute, remote admin, OTA of the radio, file transfer.
- Multi-radio simultaneous connections (one active radio at a time; multiple
  *saved* is in scope, switching between them is in scope).

---

## 4. Platform & hardware facts (carried forward — do not re-derive)

These cost real time to learn the first time. They are requirements/constraints,
not discoveries to repeat.

| Fact | Implication for v2 |
|---|---|
| Tab5 = **ESP32-P4, no radio**. BLE comes from the onboard **ESP32-C6 over esp-hosted/SDIO** (HCI tunneled; C6 is controller, P4 is NimBLE host). | Pin the known-good `esp_hosted` (1.4.0) + `esp_wifi_remote` (0.8.5) stack from M5Tab5-UserDemo. NimBLE central role on the P4. C6 stock fw already has BT — no reflash. |
| **FromNum GATT notifications never arrive** over the esp-hosted path (CCCD subscribe is *confirmed* by the radio, but `NOTIFY_RX` stays 0 all session). | **Do not design around notifications.** A periodic **poll** of FromRadio is the sync engine. Subscribe to FromNum anyway (cheap, future-proof) but never depend on it. |
| A FromRadio **GATT read completion callback occasionally never fires** over SDIO. A naive single-in-flight guard then wedges forever. | The read path must have a **timeout/recovery**: if a read is in flight > ~1.5 s, abandon it and re-issue. This is mandatory, not optional. |
| ATT **MTU negotiates to 255** reliably; the old "MTU race" was a red herring. Default 23 truncates large packets. | Request a large MTU (512) and gate the first reads until MTU resolves. Don't `read_long` — Meshtastic pops a fresh packet per read, so long-reads splice packets. |
| Meshtastic dedupes ToRadio writes. | `want_config_id` (and any retried write) must carry an **incrementing** id. |
| **Attaching serial to the P4 (`/dev/ttyACM*`) resets it.** | Cannot rely on USB serial for live debugging. Need **on-screen diagnostics** (behind a debug flag) as the primary observability surface. |
| **Cold boot from battery ≠ warm serial-reset boot.** A regression (the I²C keyboard) cold-boot-failed while warm dev boots masked it. **Battery must be plugged in to run/flash** (USB alone browns out). | Every "does it boot" claim **must be validated on a real cold/battery boot**, not a `idf.py monitor` reset. CI of "it boots" is a human pulling power. |
| Landscape **1280×720 via PPA hardware rotation** in display init. Runtime LVGL rotation (`board.lvgl_init()` + rotate) **corrupts the render**. | Reuse the proven PPA-rotation display init pattern. Do not use runtime rotation. |
| Port assignment is **plug-order dependent** (`/dev/ttyACM0` vs `ACM1`). | Document the `udevadm ... SERIAL_SHORT` check in the README. Tab5(P4)=`30:ED:A0:E1:5F:EB`. |

### Reference test node
M5Stack **Unit C6L** (`hwModel M5STACK_C6L`, fw 2.8.0), BLE name `Meshtastic_0be4`,
addr `XX:XX:XX:XX:XX:XX`, FIXED_PIN `123456`, LONG_FAST, region UNSET (BLE/config
sync works regardless of region). Its USB serial works
(`meshtastic --port <dev> ...`) — useful for cross-checking what the radio reports.

### Meshtastic BLE GATT (carried forward)
- Service `6ba1b218-15a8-461f-9fa8-5dcae273eafd`
- **ToRadio** (write) `f75c76d2-129e-4dad-a1dd-7866124401e7` — raw `meshtastic_ToRadio` protobuf
- **FromRadio** (read) `2c55e69e-4993-11ed-b878-0242ac120002` — raw `meshtastic_FromRadio`; returns 0 bytes when the queue is empty
- **FromNum** (read/notify) `ed9da18c-a800-4f66-a670-aa7547e34453` — CCCD subscribe confirmed but notifications don't traverse the tunnel
- ToRadio/FromRadio require an **authenticated, encrypted** link (bond first).
- Handshake: connect → bond (PIN) → MTU exchange → discover service/chars →
  subscribe FromNum → write ToRadio `want_config_id` → drain FromRadio until
  `config_complete_id` echoes the id → steady state (poll drains live packets).

---

## 5. Architecture principles

The previous build coupled BLE state, protocol decoding, and LVGL together and
grew brittle. v2 enforces strict layering with **one rule above all: the BLE/
protocol layer never touches LVGL, and the UI layer never makes BLE calls except
through a narrow command interface.**

```
┌─────────────────────────────────────────────────────────┐
│  UI layer (LVGL task only)                                │
│  - screens/widgets, on-screen keyboard, navigation       │
│  - reads an immutable snapshot of AppState                │
│  - emits Commands (connect, send_text, select_device…)   │
└───────────────▲───────────────────────┬──────────────────┘
                │ snapshot (mutex/copy)  │ Command queue
┌───────────────┴───────────────────────▼──────────────────┐
│  AppState / Controller  (single source of truth)         │
│  - node DB, message log, connection state, settings      │
│  - owns the state machine; applies events & commands     │
└───────────────▲───────────────────────┬──────────────────┘
                │ Events (decoded)       │ Actions (encoded)
┌───────────────┴───────────────────────▼──────────────────┐
│  Meshtastic protocol layer (pure, testable)              │
│  - nanopb encode/decode FromRadio/ToRadio                │
│  - want_config sequencing, packet→event mapping          │
│  - NO BLE, NO LVGL — operates on byte buffers            │
└───────────────▲───────────────────────┬──────────────────┘
                │ bytes in               │ bytes out
┌───────────────┴───────────────────────▼──────────────────┐
│  BLE transport (NimBLE central, esp-hosted)              │
│  - scan, bond, GATT discovery, MTU, the poll/drain engine │
│  - read-timeout recovery, reconnect/heal state machine    │
└──────────────────────────────────────────────────────────┘
         board / display (PPA-rotation init) · NVS storage
```

Key rules:
- **Single source of truth.** All mutable app state lives in one `AppState`
  guarded by one mutex. UI renders from a snapshot; nothing else holds shadow
  copies.
- **Thread boundaries are explicit.** BLE work runs on the NimBLE host task. All
  LVGL work runs on the LVGL task. They communicate only via the snapshot (BLE→UI)
  and a command queue (UI→BLE). Document this at the top of each module, enforce
  it in review.
- **The protocol layer is pure and unit-testable** off-device against captured
  byte buffers — no BLE, no LVGL, no FreeRTOS in its core.
- **No global state machine spread across files.** One connection state machine,
  one place, with explicit named states and transitions.
- **Observability is built in, not bolted on.** A structured diagnostics record
  (counters + last error + current state) is part of `AppState` and rendered by
  an optional on-screen debug overlay.

---

## 6. Functional requirements

### 6.1 Onboarding & device management (headline)
- **FR-1.1** First boot with no saved device shows a **Welcome** state with "Scan
  for devices."
- **FR-1.2** **Discovery**: active BLE scan listing Meshtastic devices (name,
  address, signal strength, "paired" tag if previously bonded). Rescan control.
- **FR-1.3** **PIN entry**: 6-digit entry with on-screen numeric keypad. States:
  entering / bonding / error (wrong PIN, timeout, busy).
- **FR-1.4** On successful bond + first sync, **persist the device** (address +
  chosen PIN + friendly name) to **NVS**. Bonds persist via NimBLE's bond store;
  the selected address/PIN get their own NVS keys.
- **FR-1.5** **Device manager**: list saved devices; connect/switch to one, or
  forget one (which also clears its bond). "Add device" re-enters discovery.
- **FR-1.6** On normal boot with a saved active device, **auto-connect** to it
  without user interaction.

### 6.2 Connection & status
- **FR-2.1** A persistent **status bar**: my node badge/name/id, link-state chip
  (signal + label), node count, battery, clock.
- **FR-2.2** Connection states surfaced to the user: `no device → scanning →
  selecting → entering PIN → bonding → connecting → syncing → READY`, plus error
  states (wrong PIN, busy, out of range, disconnected).
- **FR-2.3** A **sync banner** during the initial config download (it can take
  ~15–25 s for a large node DB).
- **FR-2.4** **Self-healing**: stalls during sync or steady state trigger
  automatic recovery (abandon wedged read → re-issue; re-issue `want_config` with
  a new id; on repeated failure, tear down and reconnect; on persistent bond
  failure, unpair and re-bond). The user sees state changes, never a silent hang.

### 6.3 Nodes
- **FR-3.1** **Node list**: scrolling rows — badge, long name, id, signal bars,
  SNR dB, hop pill (DIRECT / N hops), last-heard. Live updates as NodeInfo arrives.
- **FR-3.2** Repaint only on meaningful change (topology / new node / changed
  fields), **not** on SNR jitter — a busy mesh re-reports NodeInfo constantly.
- **FR-3.3** **Sort** by Last-heard / SNR / Hops (header control).
- **FR-3.4** **Node detail view** (headline): tap a row → detail view with badge,
  name, id; cards for SNR / hops / last-heard / battery; GPS/position line;
  device metrics (voltage, channel utilization, uptime where available). Decode
  and render `Position` and `DeviceMetrics`/`Telemetry` from the protos.

### 6.4 Chat (table-stakes baseline)
- **FR-4.1** **Receive**: decode incoming `TEXT_MESSAGE_APP` and append to a
  message log; render as bubbles (received = left/surface with sender short-name).
- **FR-4.2** **Send**: compose with an on-screen keyboard + send broadcast on the
  primary channel; show the sent message via local echo (own broadcast doesn't
  loop back from the radio).
- **FR-4.3** Chat updates on a **snappy path** (new message appears within ~1
  poll, ~600 ms), and rendering a new message must **not** rebuild the whole list
  or sit on the steady-state redraw path.
- **FR-4.4** Disable cursor blink animation on the composer (it churns full
  refreshes).

### 6.5 Diagnostics (developer-facing)
- **FR-5.1** An optional on-screen **diagnostics overlay** (behind a build/runtime
  flag, hidden in "release") showing connection stage + counters: MTU, conn
  interval, reads-with-data, NodeInfo count, decode failures, poll ticks,
  abandoned reads, CCCD-subscribe confirmed, want_config acked, last GATT error.

---

## 7. Non-functional requirements

- **NFR-1 (Stability):** Cold boot from battery succeeds on **20/20** consecutive
  power-cycles before any feature is called "done." No render corruption, no
  brownout-triggered hang, no watchdog reset.
- **NFR-2 (Connection reliability):** From a saved device, reach `READY` within
  ~25 s on ≥ 9/10 cold connects, with zero permanent "syncing" hangs across an
  extended soak (e.g. 1 hr steady-state with periodic radio range loss).
- **NFR-3 (UI responsiveness):** No visible jank during steady state. The node
  list and chat must not full-refresh on every poll. Target a smooth ~30 fps
  LVGL render; input (touch, keyboard) feels immediate.
- **NFR-4 (Memory):** Bounded memory — node DB and message log are fixed-size
  ring buffers (size them for ~200+ nodes; messages e.g. 64-deep). No unbounded
  growth in a long soak. Use PSRAM deliberately for large buffers.
- **NFR-5 (Recoverability):** Any single dropped GATT op, lost callback, or
  transient disconnect recovers automatically without user action.
- **NFR-6 (Maintainability):** Layer boundaries (§5) are enforceable by reading
  one module; the protocol layer is unit-testable off-device; adding a new
  FromRadio variant or a new screen touches one layer.
- **NFR-7 (Observability):** Sync/connection state is always readable on-device
  (no serial required), because attaching serial resets the P4.

---

## 8. UI / UX design language (carried forward)

Reuse the established dark design system (source: `../Tab5-Meshtastic/design/`).
1280×720 landscape.

**Palette**
| role | hex |
|---|---|
| bg base | `#0C0F0E` |
| chrome (rail/statusbar/composer) | `#0A0D0C` |
| surface / key / off-bar | `#1C2320` |
| surface alt (cards/inputs) | `#161C19` |
| panel (overlay) | `#0F1412` |
| accent green | `#58D98A` |
| green ink (on green) | `#07120C` |
| warn amber | `#F2C14E` |
| error red | `#EC6A5A` |
| text hi / mid / dim | `#E9EFEC` / `#93A09A` / `#5E6A64` |
| hairline / border | `#2C352F` / `#1C2320` |

**Type:** Montserrat on device — 26 title · 18 row/body · 16 message · 13 meta.
Weights 600–800.

**Metrics:** 8px grid (4/8/12/16/24) · radii 9/12/14, pills 7 · **min touch 48** ·
rail width 88 (btn 64) · status bar 56 · list row 58.

**Signal indicator** (densest element): 4 vertical bars by SNR bucket + numeric
dB in the same color + hop pill.
- ≥ −5 dB → 4 bars green · −5…−12 → 3 bars green · −12…−17 → 2 bars amber ·
  < −17 → 1 bar red. Hop pill: 0 → "DIRECT" green; else "N hops" neutral.

**Screen inventory (v1):** App shell (left rail: NODES / CHAT / RADIO; status bar;
content) · Nodes tab · Node-detail view · Chat tab · Onboarding (Welcome /
Discovery / PIN / Device manager) · optional Diagnostics overlay. (Spec tab and
channel tabs are out of v1 scope.)

---

## 9. Reliability & stability strategy (the centerpiece)

Because "device unstable" and connection flakiness are the top pains, the rebuild
codifies these as load-bearing mechanisms, not afterthoughts:

1. **Poll-driven drain, not notification-driven.** A single periodic task (~600
   ms) is the only thing that advances sync and pulls steady-state packets. FromNum
   is subscribed but never trusted.
2. **Read-timeout recovery is in the core loop.** Every FromRadio read is tracked
   with an issue timestamp; > ~1.5 s in flight → abandon + re-issue. This is the
   fix for the lost-callback wedge and must exist from day one.
3. **One explicit connection state machine** with a watchdog: progress is
   measured (packets/nodes since last tick); a stall escalates through re-read →
   re-`want_config` → reconnect → unpair/re-bond. Define max retcounts and
   backoff explicitly.
4. **MTU-gated first read** + incrementing `want_config_id` on every (re)issue.
5. **Cold-boot discipline.** Any change that touches board bring-up, peripherals,
   power rails, or the boot path is validated on a **real cold battery boot**
   (capture a cold-boot serial log) before merge. This is the lesson from the
   reverted I²C keyboard.
6. **Bounded everything** (NFR-4) so a multi-hour soak can't OOM.
7. **Built-in diagnostics** (FR-5.1, NFR-7) so failures are diagnosable on-device.

**Acceptance gates tie back here:** NFR-1 (20/20 cold boots), NFR-2 (9/10 connects,
1 hr soak, no permanent hang) are the gates for declaring the connection layer
done.

---

## 10. Data & protocol layer

- **Protos:** reuse generated nanopb (0.4.9.1) Meshtastic protos as a vendored
  component (`mesh`, `config`, `channel`, `telemetry`, `portnums`, `mesh_packet`,
  etc.). Regenerate only if upgrading proto versions.
- **Decode (FromRadio variants we act on in v1):** `my_info` (MyNodeInfo),
  `node_info` (NodeInfo → name, SNR, hops, **position**, **device_metrics**),
  `channel` (primary only), `config*` (logged), `config_complete_id`, and
  `packet` → `Data` with portnum `TEXT_MESSAGE_APP` (0x64) and `POSITION_APP` /
  `TELEMETRY_APP` for node detail.
- **Encode (ToRadio):** `want_config_id` (incrementing), broadcast
  `TEXT_MESSAGE_APP`.
- **AppState records:** `Node{ num, long/short name, snr, hops, last_heard,
  position?, metrics?, has_user }`, `Message{ from, text, is_self, recv_tick }`,
  `Connection{ state, my_node, mtu, conn_interval, diag_counters }`,
  `Settings{ active_device, saved_devices[] }`.

---

## 11. Milestones / build order

Each milestone ends with an on-hardware, **cold-boot-validated** check.

- **M0 — Skeleton & boot.** New project, pinned esp-hosted/wifi_remote stack,
  PPA-rotation display init, empty LVGL shell + status bar, NVS init.
  *Gate:* 20/20 cold boots, shell renders, no corruption.
- **M1 — BLE transport + sync engine.** NimBLE central, scan, bond (still
  hardcoded test node for now), GATT discovery, MTU, **poll/drain with
  read-timeout recovery**, want_config → config_complete. Diagnostics overlay.
  *Gate:* full node-DB sync to READY, NFR-2 soak passes.
- **M2 — Protocol layer + node list.** Pure decode/encode layer with off-device
  unit tests; AppState node DB; Nodes tab live + sort.
  *Gate:* node list matches `meshtastic --info` from the radio's serial.
- **M3 — Node detail + telemetry (headline).** Decode Position/DeviceMetrics;
  detail view.
- **M4 — Chat baseline.** Receive/send broadcast text; bubbles + composer + OSK;
  snappy append path.
- **M5 — Onboarding / device picker (headline).** Welcome / Discovery / PIN /
  Manager; runtime device selection; NVS persistence; auto-connect on boot.
  Remove all hardcoded address/PIN.
  *Gate:* pair to the test node from a clean NVS, power-cycle, auto-reconnects.
- **M6 — Polish & release.** Hide diagnostics behind flag, soak test, README.

**Deferred / gated:** Physical I²C keyboard — only revisit after M6, and only with
a cold-boot validation harness in place (it previously regressed cold boot while
warm dev boots hid it). Channels, DMs, acks, synced timestamps → v2.

---

## 12. Open questions / risks

- **R1 — esp-hosted notification gap is structural.** If a future feature truly
  needs server-initiated notifications, it requires digging into esp-hosted/C6
  slave internals (does the slave forward GATT notifications at all?). Out of
  scope for v1; poll covers everything v1 needs.
- **R2 — Synced wall clock.** No RTC sync means no real per-message timestamps /
  ordering across reboots. Could set time from the radio's position/time packets
  later; deferred. Affects chat timestamps + last-heard absolute times (use
  relative "Xm ago" for now).
- **R3 — "Device unstable" root cause unconfirmed.** Need to determine whether the
  past instability was purely the I²C keyboard / boot-path change, or something in
  the power/peripheral bring-up. M0's 20/20 cold-boot gate is the first probe; if
  a clean skeleton is stable, instability was self-inflicted by later changes.
- **R4 — Proto version drift.** Radio fw 2.8.0 protos vs. vendored protos —
  confirm field/tag compatibility (note from v1: FromRadio variant 9=moduleConfig,
  13=metadata, 17=deviceuiConfig).
- **R5 — Reusing vs. re-deriving the sync engine.** Green-field per decision, but
  the v1 `poll_cb`/`try_drain` logic in `../Tab5-Meshtastic/main/main.cpp`
  (lines ~142–157, 499–561) is the reference contract for the recovery behavior —
  re-implement its *guarantees*, not necessarily its code.

---

## 13. Reference material

- `../Tab5-Meshtastic/STATUS.md` — detailed v1 handoff (sync internals appendix).
- `../Tab5-Meshtastic/README.md` — GATT UUIDs, build/flash, hardware setup.
- `../Tab5-Meshtastic/design/` — `DESIGN_SPEC.md` + `Tab5_Meshtastic.dc.html` mockup.
- `../Tab5-Meshtastic/main/{main.cpp,ui.cpp,lcd_tools.cpp}` — working v1 backend,
  UI, and proven display init.
- `../firmware/` — upstream Meshtastic firmware (server-side BLE reference:
  `src/nimble/NimbleBluetooth.cpp`, `src/mesh/PhoneAPI.{h,cpp}`,
  `src/BluetoothCommon.h`).
- `../M5Tab5-UserDemo/` — known-good esp-hosted/SDIO + BSP baseline.
