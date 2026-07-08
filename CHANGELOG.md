# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed

- **Remote Mode command used the wrong GENIbus opcode and never actually took effect** —
  `enable_remote_mode()`/`disable_remote_mode()` sent Class 3 command
  `[0x03, 0xC1, ...]` (OpSpec 0xC1 = INFO). Bench-verified against a real
  pump (2026-07-08) that this opcode is always rejected with a
  "descriptor-only" ACK (`[03 01 AC]`), while OpSpec `0x81` (SET) reliably
  produces the clean success ACK (`[03 00]`) — confirmed by sending both
  variants (and both the `0xF8`/`0x0A` source-address theories) back-to-back
  and comparing the raw ACK bytes, ruling out the source address as a factor.
  Switched both commands to `0x81`. Also added `Transport` support for
  matching short Class 3 ACK responses (previously only Class 7/10 responses
  were dispatched to command callbacks; Class 3 ACKs as short as 8 bytes were
  silently discarded before even reaching a length check), so
  `is_remote_mode_enabled_` is now only updated once the pump's ACK actually
  confirms success, instead of being set unconditionally right after sending
  the command
  (issue [#46](https://github.com/eman/esphome-alpha-hwr/issues/46)).
- **Memory leak in schedule "read all layers" async chain** —
  `ScheduleService::read_entries_async(-1, ...)` drove its layer-by-layer read
  loop with a self-referential `std::shared_ptr<std::function>`. `Transport::reset()`
  (called on every BLE disconnect) clears the pending command queue without
  invoking callbacks, so a disconnect landing mid-read left the self-reference
  unbroken and leaked the closure (cached entries, completion callback, and all).
  Rewritten using a stateless recursive lambda plus a plain, non-cyclic state
  object, so an interrupted read now cleans up normally regardless of when the
  disconnect happens (issue [#32](https://github.com/eman/esphome-alpha-hwr/issues/32),
  PR [#38](https://github.com/eman/esphome-alpha-hwr/pull/38)).

### Changed

- **Centralized GENI frame construction** — added `Transport::send_apdu_command()`;
  services no longer manually call `build_geni_packet()` and hardcode the
  transport-level Service ID/Source Address themselves, keeping protocol framing
  out of the services layer per the project's layered architecture
  (issue [#28](https://github.com/eman/esphome-alpha-hwr/issues/28),
  PR [#36](https://github.com/eman/esphome-alpha-hwr/pull/36)).
- **Schedule JSON generation moved into `ScheduleService`** —
  `ScheduleService::generate_json()` now owns building the compact JSON used by
  the Weekly Schedule text sensor; `AlphaHwrComponent` no longer reaches into
  `ScheduleService`'s internal cache directly
  (issue [#29](https://github.com/eman/esphome-alpha-hwr/issues/29),
  PR [#36](https://github.com/eman/esphome-alpha-hwr/pull/36)).
- **Consolidated schedule write encoding** — `write_entries()` and
  `write_entries_async()` now share a `build_schedule_apdu()` helper instead of
  duplicating the per-day entry encoding logic
  (issue [#30](https://github.com/eman/esphome-alpha-hwr/issues/30),
  PR [#41](https://github.com/eman/esphome-alpha-hwr/pull/41)).
- **Reduced `ScheduleEntry` memory footprint** — replaced the `std::string day_`
  field with a compact `uint8_t` day index (falling back to a sentinel for
  unrecognized day names instead of silently treating them as valid), and
  replaced a `std::map`-based per-day/layer entry count in schedule validation
  with a fixed array
  (issues [#31](https://github.com/eman/esphome-alpha-hwr/issues/31),
  [#34](https://github.com/eman/esphome-alpha-hwr/issues/34), PRs
  [#41](https://github.com/eman/esphome-alpha-hwr/pull/41),
  [#42](https://github.com/eman/esphome-alpha-hwr/pull/42)).

## [0.7.0] - 2026-07-05

### Added

- **Pump Link Status and Pump Link Fault text sensors** — two optional text
  sensors surfacing BLE link health. `Pump Link Status` reports a coarse
  connection-state enum (`Initializing`, `Connecting`, `Reconnecting`,
  `Connected`, `Unpaired`, `Unreachable`); `Pump Link Fault` shows the latched
  human-readable reason for the most recent failure (e.g. `Encryption Start
  Failed (0x61)`, `Remote Terminated (0x13)`), clearing to `None` once the link
  returns to `Connected`. The fault deliberately holds a significant
  auth/encryption failure through the reconnect churn so the cause survives to
  be seen without logs. Aimed at self-support, triage, and monitoring/automation
  (issue [#21](https://github.com/eman/esphome-alpha-hwr/issues/21), PR
  [#22](https://github.com/eman/esphome-alpha-hwr/pull/22)).

## [0.6.0] - 2026-07-03

### Added

- **`reconnect_settle_time` option** — device-agnostic fix for bond loss after
  a pump restart (issue [#5](https://github.com/eman/esphome-alpha-hwr/issues/5),
  PR [#11](https://github.com/eman/esphome-alpha-hwr/pull/11)). After a
  power-cycle the ESP32 could reconnect the instant the pump advertised, before
  its BLE stack was ready; the on-open encryption request then failed with
  `0x61` and ESP-IDF silently erased the stored bond. When set, the component
  waits for the pump to *reappear* after a disconnect and holds off
  reconnection for the configured settle time — timed from reappearance, so a
  5-second cycle and a 5-minute outage behave identically. The window applies
  only when a bond exists, so initial pairing is never delayed. Defaults to
  `2s`, which covers the measured 320-720ms pump vulnerability window with
  ~2.8x margin independent of host-side timing (issue
  [#14](https://github.com/eman/esphome-alpha-hwr/issues/14)); set `0s` to
  disable and restore the previous immediate-reconnect behavior.
- **Pump advertisement decoding at scan time** — new `PumpAdvertisementInfo`
  decoded from raw BLE advertisement bytes before any GATT connection, exposing
  `product_family`, `product_type`, and `product_version` (the BLE firmware
  discriminator between pump hardware revisions) plus the raw advertisement hex
  for debugging (PR [#10](https://github.com/eman/esphome-alpha-hwr/pull/10)).

### Fixed

- **Failed BLE opens no longer treated as real connections** — the base layer
  forwards `ESP_GATTC_OPEN_EVT` to components even when the open failed, so
  with the pump powered down or out of range the reconnect loop's stream of
  failed opens drove phantom `IDLE -> SERVICE_DISCOVERY` transitions, misfired
  encryption requests against dead connections, and could defeat the
  `reconnect_settle_time` window. The handler now runs only for successful
  opens (`ESP_GATT_OK` or `ESP_GATT_ALREADY_OPEN`, mirroring the base layer's
  success condition) and log-and-ignores the rest
  (PR [#9](https://github.com/eman/esphome-alpha-hwr/pull/9)).
- **`is_alpha_hwr_device()` matched the wrong advertisement field** — the check
  read Manufacturer Specific data at bytes 2/3, but the pump advertises its
  identity via Service Data (AD type `0x16`); detection now parses Service Data
  at the offsets used by the Python reference
  (PR [#10](https://github.com/eman/esphome-alpha-hwr/pull/10)).
- **CCCD write raced BLE encryption negotiation on bonded reconnect** — on
  fast-BLE chips, service discovery could complete mid-SMP-negotiation, so the
  notification-subscription (CCCD) write fired unencrypted and raced
  `ESP_GAP_BLE_AUTH_CMPL_EVT`; a rejection could end in bond erasure. The
  subscription is now deferred until `AUTH_CMPL` succeeds whenever an
  encryption request is still pending, tracked by two connection-scoped flags
  that reset on open, auth failure, and disconnect
  (issue [#12](https://github.com/eman/esphome-alpha-hwr/issues/12), PR
  [#13](https://github.com/eman/esphome-alpha-hwr/pull/13)).
- **Stabilize-to-auth timer survived a disconnect landing before auth started**
  — a disconnect inside the ~2s reconnect-settle window (before
  `authenticate()` had run) left the anonymous stabilize timer pending, so it
  later fired against whatever connection existed next. The timer is now named
  (`hwr_auth_start`) and explicitly cancelled on disconnect alongside
  `auth_.cancel()` (issue [#15](https://github.com/eman/esphome-alpha-hwr/issues/15),
  PR [#17](https://github.com/eman/esphome-alpha-hwr/pull/17)).
- **Initial-read-chain timers survived a disconnect mid-chain** — the post-auth
  chain of reads (device info, statistics, clock sync, schedule display,
  setpoints, event log/history/single-events) used anonymous `set_timeout`
  calls that kept firing against a new connection if a disconnect landed
  during the chain. A `read_chain_gen_` generation counter, bumped on
  disconnect, is now captured by every timer lambda at schedule time; each
  returns early if the generation no longer matches, mirroring the
  self-invalidation pattern already used by `auth_sequence_` and
  `scheduler_sequence_` (issue [#18](https://github.com/eman/esphome-alpha-hwr/issues/18),
  PR [#19](https://github.com/eman/esphome-alpha-hwr/pull/19)).

---

## [0.5.0] - 2026-06-30

### Fixed

- **BLE reconnect stability after pump power-cycle** — three bugs combined to
  produce a wedged reconnect loop that could only be recovered by reflashing
  ([#5](https://github.com/eman/esphome-alpha-hwr/issues/5), PR [#6](https://github.com/eman/esphome-alpha-hwr/pull/6)):
  - *Bond erasure on power-cycle*: encryption was requested at connection-open
    before the pump's BLE security stack was ready; a failed attempt caused
    ESP-IDF to silently erase the stored bond. Fixed by gating the encryption
    request on a bond-list check — only bonded devices request encryption
    immediately; unbonded devices defer to the pump's own `SEC_REQ`.
  - *Unreliable initial pairing*: a central-initiated pairing request on an
    unbonded pump returns `0x52 "Pairing Not Supported"`, causing the ESP32 to
    miss the pump's `SEC_REQ`. Fixed by the bond-check above.
  - *Stale state wedges reconnect*: `on_disconnected()` did not cancel the
    in-flight authentication handshake, leaving pending scheduler lambdas
    firing into the new BLE connection ("Stage 3: Sending extension packets"
    before the connection was open, "Service already running" on next
    auth-complete). Fixed by calling `auth_.cancel()`,
    `telemetry_service_.stop()`, resetting `initial_data_read_done_`, and
    flushing the transport command queue and pending response handlers on every
    disconnect.
- **Deprecated `ESPBTUUID::to_string()` replaced with `to_str()`** — two
  `ESP_LOGW` calls in `ble_connection_manager.cpp` used the deprecated
  `to_string()` method that ESPHome will remove in 2026.8.0
  ([#4](https://github.com/eman/esphome-alpha-hwr/issues/4), PR [#7](https://github.com/eman/esphome-alpha-hwr/pull/7)).
- **Spurious warnings for unsupported trend channels** — some pump models
  (e.g. ALPHA HWR 15-290 SU/T) do not populate the Temperature trend channel
  (Object 53 SubID 453), generating two `WARN`-level log lines on every
  startup. Wildcard command timeouts in the transport layer are now `DEBUG`;
  the trend-read timeout is reduced from 3 000 ms to 1 500 ms.

---

## [0.4.0] - 2026-05-18

### Added

- **DHW Demand Detector** (`dhw_demand` component) — standalone ESPHome
  component that detects genuine domestic-hot-water demand events by fusing
  pump telemetry with supplementary sensors (Droplet D1 flow meter, tank
  temperature, NWP500 charge/in-use flag). Uses heuristic, threshold-based
  voting with no ML or external dependencies. Fully tunable via YAML
  `substitutions` without reflashing.
- **Derived pressure sensors** — inlet pressure and head-rate (kPa/s) computed
  from pump telemetry, exposed as Home Assistant sensors.
- **`dhw-demand-example.yaml`** and **`packages/dhw_demand_detector.yaml`**
  package for drop-in DHW demand detection.
- Protocol hardening and robustness improvements (Protofix commit).

### Fixed

- Motor enabled state separated from motor *running* state — pump-enabled
  binary sensor now reflects firmware state rather than RPM > 0.
- Head-rate computation refactored to callback-based approach; dt reset
  threshold raised from 3 s to 30 s to prevent false spikes on reconnect.
- Unit reporting corrections across multiple sensors.
- `esp32_ble_tracker` key restored in `alpha_hwr_base.yaml` (broken package
  after package restructure).
- `motor_speed` sensor ID corrected in `hwr-pump.yaml`.
- `cppcheck` static analysis findings resolved across the component.

---

## [0.3.0] - 2026-02-22

### Added

- **Event log service** — reads the last 20 pump cycle events (start/stop
  timestamps, cycle numbers) on startup.
- **History trends service** — reads flow, head, temperature, and power-on-time
  trend data (last 10 and 100 pump cycles) from Object 53.
- **Operating statistics** — start count and total operating hours read from
  the pump on startup.
- **Cycle timestamps** — last 10 and 100 cycle Unix timestamps read from
  Object 88.
- **Quick Run / one-time schedules** — `quick_run_async()` method and
  Home Assistant button for immediate one-time pump activation.
- **Schedule management Lovelace card** — custom card for displaying and
  editing the weekly schedule directly in the Home Assistant dashboard.
- **ESPHome API services** — `alpha_hwr.set_schedule`, `alpha_hwr.quick_run`,
  and `alpha_hwr.sync_clock` callable from automations.
- **GENI error code descriptions** — human-readable labels for all pump alarm
  and warning codes in the Home Assistant UI.
- **Proportional Pressure control** — `set_proportional_pressure_async()`
  with m→Pa conversion matching the Python reference.
- **Cycle Time control** — `set_cycle_time_control_async()` for Object 91
  Sub 430 structured write.
- Boot-resilient initial data reads — device info, clock sync, event log, and
  history are re-triggered on reconnect if not yet completed.

### Fixed

- **Packet format for time sync** — rewrote `TimeService::set_clock_async()`
  to use proper Class 10 SET (`build_data_object_set`) with Type 322 header,
  replacing incorrect Class 7 style frame. Changed to fire-and-forget to
  eliminate 5-second transport queue blocking on every update cycle.
- **OpSpec 0x09 alarm/warning handling** — added handler for register-read
  response format; uses request-register echo to route alarms vs. warnings
  deterministically instead of poll-order toggle.
- **Duplicate frame builder** eliminated; CRC bug in control service packet
  builder fixed.
- Flash usage reduced by removing development logging (75.8% → within limits).
- Schedule display timeout and select-entity lag fixed.
- Pump switch now reads real state from passive notifications instead of
  assuming state after command.
- Setpoints (temperature range, flow, pressure) now read from passive
  notifications (Object 0x2F01) instead of failing Object 86 queries.
- Cycle time control wildcard response matching (pump's OpSpec 0x15 response
  does not carry Object/Sub IDs at standard bytes 6-7/8-9).

---

## [0.2.0] - 2026-02-15

### Added

- **Weekly schedule management** — read and write all 5 schedule layers;
  schedule display as a formatted text sensor in Home Assistant.
- **Device information service** — reads serial number, software version,
  hardware version, BLE version, and product name via Class 7 string commands;
  exposed as diagnostic text sensors.
- **Real-time clock service** — reads pump RTC (Object 94 Sub 101) and writes
  it (Object 94 Sub 100) once per day automatically after SNTP sync.
- **Control mode text sensor** — shows the pump's actual current control mode
  sourced from passive notifications (Object 0x2F01 Sub 1, OpSpec 0x0E);
  never shows a default/fake value before the pump reports its real state.
- **`packages/alpha_hwr_controls.yaml`** — optional package with all control
  UI entities (sliders, selects, buttons).
- **`hwr-pump-schedule-example.yaml`** — example configuration for schedule
  management.

### Fixed

- **Control service alignment with Python reference**:
  - `CONSTANT_FLOW` mode byte: `0x00` → `0x08`.
  - DHW On/Off suffix bytes corrected.
  - Temperature Range APDU size field: `0x09` → `0x0D`.
  - Constant Pressure now converts m → Pa (`× 9806.65`).
  - Constant Flow max range: 5.0 → 10.0 m³/h.
- **`set_mode()` simplified** — removed incorrect Class 3 fallback; pump
  always uses Class 10 for mode changes.
- **Object/Sub-ID byte order** in `Transport::try_dispatch_response()` — bytes
  6-7 are Object ID, bytes 8-9 are Sub-ID (was reversed).
- **Class 7 response matching** — matched by class byte only when
  `expect_obj_id == 0 && expect_sub_id == 0`, fixing device info reads.

---

## [0.1.0] - 2026-02-11

### Added

- Initial public component release.
- **BLE discovery** — identifies ALPHA HWR pumps by Grundfos Company ID
  (`0x0059`) with product family/type byte validation; falls back to service
  UUID (`0xFE5D`) detection.
- **GENI protocol authentication** — 3-stage handshake: legacy magic burst
  (3×), Class 10 unlock burst (5×), extension packets (EXT_1 + EXT_2).
- **Live telemetry streaming** — motor state (RPM, current, power, AC/DC
  voltage), flow rate, inlet pressure, media/PCB/box temperatures, alarms,
  and warnings; polled every 10 seconds.
- **Bidirectional pump control** — Start/Stop, mode selection (Auto, Constant
  Flow, Constant Pressure, Proportional Pressure, Temperature Range, Cycle
  Time, DHW On/Off), and setpoint adjustment.
- **Pairing support** — BLE bonding with `esp_ble_set_encryption()` and
  `ESP_GAP_BLE_SEC_REQ_EVT` acceptance; pairing status binary sensor.
- **Non-blocking BLE transaction manager** — command queue
  (`std::deque<Command>`) with 3-state FSM (`IDLE`, `SENDING_CHUNKS`,
  `AWAITING_RESPONSE`), 50 ms inter-packet pacing, and response matching by
  Object ID / Sub-ID with `SubID 0` wildcard quirk handling.
- **Layered, service-based architecture** mirroring the Python reference
  implementation: `core::` (transport, session, auth, BLE manager),
  `protocol::` (codec, frame builder/parser, telemetry decoder),
  `services::` (telemetry, control, schedule, sensor publisher).
- **Package-based YAML configuration** — `packages/alpha_hwr_base.yaml` and
  `packages/alpha_hwr_pairing.yaml` for modular device configs.
- **`hwr-pump-example.yaml`** and **`hwr-pairing-example.yaml`** reference
  configurations.

[Unreleased]: https://github.com/eman/esphome-alpha-hwr/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/eman/esphome-alpha-hwr/releases/tag/v0.1.0
