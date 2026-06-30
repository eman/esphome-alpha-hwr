# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/eman/esphome-alpha-hwr/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/eman/esphome-alpha-hwr/releases/tag/v0.1.0
