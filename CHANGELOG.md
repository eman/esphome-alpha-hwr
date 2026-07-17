# Changelog

## [Unreleased]

## [0.11.0] - 2026-07-17

### Added

- **"Component Version" diagnostic entity** —
  A new diagnostic text sensor reports this ESPHome component's own release
  version (distinct from the pump-firmware Software/Hardware/BLE/Product Version
  entities). It is recorded in Home Assistant history, so the component version
  that produced any state is answerable after the fact and on bug reports. The
  value is kept in sync automatically by `tools/bump_version.sh`
  (fixes [#95](https://github.com/eman/esphome-alpha-hwr/issues/95)).

### Fixed

- **Constant Flow setpoint entity showed the requested value, not the stored one** —
  #82 added a 1.2s post-write readback so each setpoint entity reflects the value
  the pump actually stored (e.g. when it clamps or rejects the request). Constant
  Flow was the only setter still missing it — a leftover from when flow readback
  was thought unreliable, which #88/#90 later showed was a write-units bug (m³/h
  vs the pump's native m³/s). `set_constant_flow_async` now performs the same
  readback as the other three modes, so the Constant Flow entity settles on the
  pump's real stored/clamped value (fixes
  [#96](https://github.com/eman/esphome-alpha-hwr/issues/96)).

- **Cycle-time config could permanently block all commands** —
  `is_cache_valid()` — which gates `pump_ready` and every command (start/stop, mode
  change, setpoint writes, remote mode, schedule writes) — required the pump's
  cycle-time config (Object 91), but the parser only read those bytes when the
  payload was long enough. A short or unusual Object 91 payload could leave the
  cycle-time fields at their `-1` "unknown" sentinel, making the pump report
  not-ready forever with every command silently rejected. Cycle-time config is no
  longer part of readiness (it is not displayed anywhere; same rationale as
  excluding the mode-specific setpoints). Separately, the cycle-time bytes are now
  range-validated to 1–60 on read, so a `0xFF`/`0`/out-of-range value maps to
  "unknown" instead of truncating a `uint8_t` into the `int8_t` sentinel. Both bugs
  were latent — real pumps return valid data — so this is a robustness fix against
  short or unusual Object 91 payloads. Fixes
  [#94](https://github.com/eman/esphome-alpha-hwr/issues/94).
- **Uncoordinated `current_mode_` writes during mode switches** —
  A control-mode readback that landed after a mode command was issued but before
  the pump had applied it (most easily via the 30 s out-of-band poll, but also the
  post-command reconciles) reported the *old* mode and silently overwrote the
  optimistic new mode, which then drove `sync_cache_async` into an unbounded 2 s
  retry loop that NaN-ed the setpoint cache. `set_mode`/`start` now record the
  commanded mode as *pending*, and every readback writer (`get_mode_async`, passive
  notifications) keeps the commanded mode until a readback confirms it — so a
  stale/in-flight read can no longer corrupt the mode or flicker the setpoint to
  `unknown`. The sync retry is now bounded and, if the pump never applies a command,
  falls back to accepting the pump's reported mode instead of forcing the commanded
  one forever. Out-of-band change detection (issue #54) is preserved: with no
  command pending, a readback still adopts the pump's mode. (Addresses the
  coordination race in [#91](https://github.com/eman/esphome-alpha-hwr/issues/91);
  the setpoint-clobber symptom itself was already removed by the #97/#83 fix.)
- **Three of six control modes could not be entered, and mode changes clobbered
  the stored setpoint** —
  On v0.10.3, selecting Constant Pressure, Proportional Pressure, or Constant Flow
  did nothing and aborted after a 5-second timeout; Constant Speed only worked when
  it was already the active mode. The root cause was that mode changes were sent as
  a start/stop command (GENI `overall_operation_local_request`, wire Obj 0x0601)
  that also writes the mode's *setpoint* — so the component either read the pump
  first (a read that always timed out and aborted) or fell back to a default value
  that durably overwrote the pump's stored setpoint (~3671 for Constant Speed).

  Mode switching now uses the dedicated GENI control-mode object
  (`overall_control_mode_local_request`, object 86 / sub-id 10, wire Obj 0x0A01),
  exactly as the Grundfos GO app does. Per the pump's own GENI profile this object
  changes only the control mode and ignores the run state and setpoint (the payload
  carries `operation_mode = NoCmd` and `set_point = NaN`), so every mode is
  reachable regardless of cache state and each mode's stored setpoint is preserved.
  Confirmed on-device: switching into a mode with a cold cache now leaves its
  setpoint intact (previously it was overwritten with the default). This also
  inherently prevents a mode change from force-enabling the pump. Verified against
  BLE captures and the pump's GENI profile descriptor
  (fixes [#97](https://github.com/eman/esphome-alpha-hwr/issues/97), fixes
  [#83](https://github.com/eman/esphome-alpha-hwr/issues/83), reinforces
  [#45](https://github.com/eman/esphome-alpha-hwr/issues/45)).
- **Misleading mode-change log** —
  The mode dropdown logged `Set mode result: SUCCESS` ~5 seconds before the
  asynchronous command actually resolved. It now logs that the change was *queued*,
  not that the pump applied it (issue #97).

## [0.10.3] - 2026-07-16

## [0.10.2] - 2026-07-16

### Fixed

- **Constant Flow Setpoint scaling and readback** —
  Fixed a unit mismatch where the pump's native `m³/s` Constant Flow setpoint was being passed to Home Assistant as `m³/h` without conversion. This caused the readback to display as `0.000694 m³/h` instead of `2.5 m³/h`, and caused the pump to silently reject all writes because `2.5 m³/s` (`9000 m³/h`) was severely out of range. 
  (fixes [#88](https://github.com/eman/esphome-alpha-hwr/issues/88), fixes [#81](https://github.com/eman/esphome-alpha-hwr/issues/81), roots-causes [#44](https://github.com/eman/esphome-alpha-hwr/issues/44), PR #90).
- **Constant Flow abort bug** —
  Fixed an issue in `with_resolved_setpoint` where `CONSTANT_FLOW` changes would hard-abort instead of proceeding with the setpoint write.
- **Transport Sub-ID Matching bug** —
  Fixed `send_apdu_command` calls in the control service that were inadvertently passing the Object ID as the Sub ID due to reversed argument order.
- **Prevent NVM Clobbering** —
  Introduced `with_resolved_setpoint` to query the pump for its active setpoint if it isn't cached locally. This prevents `send_control_request` from falling back to hardcoded default suffixes that clobbered the pump's NVRAM when turning it on or changing modes (fixes [#83](https://github.com/eman/esphome-alpha-hwr/issues/83), PR #89).
## [0.10.1] - 2026-07-11

### Fixed

- **Event log read timeout** —
  Fixed an issue in `EventLogService` where the expected Object and Sub-ID fields were swapped for the event log read command, causing the transport layer to drop the response and time out.

## [0.10.0] - 2026-07-11

### Fixed

- **Missing entities on reconnect / startup race conditions** —
  Added `sync_cache_async` orchestration after the session is READY 
  which polls Object 86 and Object 91 before the pump is marked as "Ready". 
  This ensures that all configuration bounds and telemetry entities populate when 
  the pump connects, eliminating stale `0`/`NaN` gaps and the need for manual polling
  (issue [#67](https://github.com/eman/esphome-alpha-hwr/issues/67)).
- **Multi-parameter writes scrambled defaults due to missing cache** —
  Because `sync_cache_async` now guarantees internal telemetry bounds (like 
  AutoAdapt and Cycle Times) are populated before writes are permitted, 
  multi-parameter setters (e.g. Temperature Range or AutoAdapt toggle) no longer
  rely on uninitialized dummy defaults `0`. The setters reuse genuine pump 
  values for unchanged fields, preventing unintended resets or scrambled schedules
  (issue [#70](https://github.com/eman/esphome-alpha-hwr/issues/70)).
- **Clock Drift reports exact timezone offset (-25,200s)** —
  Fixed an issue where the C-library `mktime` fell back to UTC parsing when reading the pump's RTC. The integration now correctly requests `time_id` and leverages ESPHome's internal `ESPTime` engine to accurately interpret the pump's local timezone
  (issue [#76](https://github.com/eman/esphome-alpha-hwr/issues/76)).

## [0.9.0] - 2026-07-10

### Added

- **Host-based unit tests and GitHub Actions CI** —
  Added a mock ESPHome harness in `tests/mocks/` to allow protocol and service
  logic to be compiled and executed natively on the host using `g++` instead of
  requiring an ESP-IDF toolchain. Added test suites for the Transport FSM and
  ScheduleService payload generation. CI is now enforced via GitHub Actions on
  pushes and pull requests to `main` (PR #59).

### Fixed

- **Remote Mode lockout and spontaneous start** —
  Fixed `disable_remote_mode` to correctly send a `LOCAL` (0x08) command rather than a
  `START` (0x06) command, which caused the pump to spontaneously run when disabling remote mode.
  Also removed the automatic `enable_remote()` call from the `pump_enabled` switch, which was
  causing the pump to ignore all commands for ~40 seconds after being turned on
  (issue [#66](https://github.com/eman/esphome-alpha-hwr/issues/66)).

- **Temperature Range writes could report success without persisting pump values** —
  `ControlService::set_temperature_range_async()` queued the Object 91/Sub 430
  write as fire-and-forget while the preceding mode-switch control request
  still auto-scheduled an early commit. That commit could run before the
  Object 91 write completed, leaving the pump on its previous/default stored
  range while Home Assistant still reported success. Temperature range writes
  now (1) disable the step-1 auto-commit for multi-step setters, (2) wait
  through a response/timeout window before committing, and (3) trigger a
  readback so cache reflects the pump's persisted state
  (plus Class 10 short ACK dispatch for the Obj 91/Sub 430 write path, and
  no success callback when the write ACK is missing),
  (issue [#65](https://github.com/eman/esphome-alpha-hwr/issues/65)).
- **Changing Temperature Range Min/Max silently re-enabled AutoAdapt** —
  The `Temperature Range Min` and `Temperature Range Max` number entities
  hardcoded `autoadapt = true` in their `set_action` lambdas, so adjusting
  either bound always turned AutoAdapt back on even if the
  `Temperature AutoAdapt` switch had just been turned off. Both setters now
  use the pump's cached AutoAdapt state (`get_cached_autoadapt()`) when
  writing the temperature range, preserving the current switch state while
  updating range bounds
  (issue [#64](https://github.com/eman/esphome-alpha-hwr/issues/64)).

- **Remote Mode switch reflected local intent rather than actual pump state** —
  The switch was declared `optimistic: false` (implying it tracks the pump's
  real state) but was driven solely by `is_remote_mode_enabled_`, a flag set
  only when our own enable/disable commands received a clean ACK. The pump's
  `control_source` byte — present in the passive Control Mode Status
  notification (Obj 0x2F01 / Sub 0x0001, OpSpec 0x0E, sent automatically
  after authentication) and in the response to the explicit Object 86 / Sub 6
  read (triggered by the periodic control-state poll, issue #54) — was parsed
  and logged but never applied to the cached state, so the switch would show
  stale data after a reconnect, a panel reset, or any external tool
  taking/releasing remote control.
  `update_mode_from_notification()` and the `get_mode_async` read callback now
  update `is_remote_mode_enabled_` when `control_source` is a recognized value
  (`2` = Remote/Digital, `1` = Local/Panel, matching the Python reference
  implementation). Unknown values (e.g. `0`, seen before the opcode fix in
  PR #50) are ignored so a stale byte cannot incorrectly clear a state that
  was confirmed by a command ACK
  (issue [#53](https://github.com/eman/esphome-alpha-hwr/issues/53)).
- **Component does not detect out-of-band pump state changes** —
  If the pump changes state autonomously (e.g., internal schedule execution,
  manual button press on the pump, external app control), the component's
  cached state diverges from the pump's actual state until the component is
  rebooted. This affects multi-control setups where exclusive component control
  cannot be guaranteed. Fixed by implementing periodic control state polling
  (Option 1 from issue discussion): a configurable background `get_mode()` read
  at regular intervals (default 30 seconds) to detect and sync state changes.
  Polling interval is configurable via the `control_state_poll_interval`
  parameter and can be disabled by setting it to 0. Users who want exclusive
  control can disable polling; those sharing pump control benefit from faster
  divergence detection.
  (issue [#54](https://github.com/eman/esphome-alpha-hwr/issues/54)).
- **Pump Enabled switch lag after toggling** —
  The pump does not send unsolicited notifications after Class 3 START/STOP
  commands, so the non-optimistic `Pump Enabled` switch in Home Assistant
  won't update from the component's cache until the next periodic telemetry
  poll (~10 seconds). This creates a user-visible lag: users toggle the switch
  and don't see confirmation until several seconds later. Fixed by scheduling
  an explicit post-command `get_mode()` readback ~500ms after every start/stop
  (reporter bench-tested this timing on real hardware and confirmed it resolves
  the UI lag reliably). The readback is non-blocking and non-critical; if it
  fails the UI simply updates on the next periodic poll as before.
  (issue [#52](https://github.com/eman/esphome-alpha-hwr/issues/52)).
- **Cross-mode setpoint contamination caused wrong setpoint on mode switches** —
  `ControlService` used a single shared `cached_setpoint_` field across all
  scalar control modes. Switching from, say, Constant Speed (2000 RPM) to
  Constant Pressure meant that 2000 was briefly the "pressure" cache, and a
  subsequent `start()` would convert it to Pascals and send ~19.6 MPa to the
  pump. Replaced the shared field with four independent per-mode caches
  (`cached_pressure_setpoint_`, `cached_proportional_setpoint_`,
  `cached_speed_setpoint_`, `cached_flow_setpoint_`); each mode reads and
  writes only its own slot, so a value set in one mode can never contaminate
  another. The mode-transition NAN-clearing workaround in `start()` is no
  longer needed and has been removed. Public getters on `ControlService` and
  the `AlphaHwrComponent` facade, plus the four number-entity lambdas in
  `packages/alpha_hwr_controls.yaml`, updated accordingly
  (issue [#51](https://github.com/eman/esphome-alpha-hwr/issues/51),
  PR [#57](https://github.com/eman/esphome-alpha-hwr/pull/57)).

## [0.8.0] - 2026-07-08

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
  were dispatched to command callbacks; Class 3 ACKs as short as 8 bytes
  were rejected by the existing 12-byte minimum-length guard before any
  class-specific matching ran), gated so a queued Class 3/7 command can only
  be satisfied by a response of that same class (an unrelated Class 10
  telemetry notification arriving first can no longer be mistaken for the
  answer). `is_remote_mode_enabled_` is now only updated once the pump's ACK
  actually confirms success, instead of being set unconditionally right
  after sending the command
  (issue [#46](https://github.com/eman/esphome-alpha-hwr/issues/46)).
- **Setpoint and mode changes unexpectedly enabled the pump** —
  `ControlService::set_mode()` and every setpoint setter
  (`set_constant_pressure_async`, `set_constant_speed_async`,
  `set_constant_flow_async`, `set_proportional_pressure_async`,
  `set_temperature_range_async`, `set_cycle_time_control_async`) hardcoded
  the GENIbus control frame's start/stop flag to "start" regardless of the
  pump's actual on/off state, so writing a setpoint or switching modes while
  the pump was off silently turned it on. Since the frame fuses mode +
  setpoint + on/off into a single write, there's no way to omit the flag —
  these calls now send the pump's actual last-known enabled state via a new
  `ControlService::with_resolved_enabled_state()` helper, which reads it back
  from the pump first if not yet known. If that read-back also fails, the
  control request is aborted entirely rather than guessing an on/off state
  (guessing "enabled" risked the original bug; guessing "disabled" would
  risk sending an explicit stop to a pump that was actually running).
  (issue [#45](https://github.com/eman/esphome-alpha-hwr/issues/45),
  PR [#49](https://github.com/eman/esphome-alpha-hwr/pull/49)).
- **Constant Flow Setpoint displayed a value off by ~1000x** —
  bench-verified against the physical pump (2026-07-08) that Class 10
  Object 86/Sub 6 returns the exact same raw value (~0.000694 m³/h)
  regardless of the actual commanded flow setpoint (tested 0.2/2.0/8.0 m³/h,
  all identical); converted to the originally reported units, that's
  `0.003056 gal/min` — an exact match. Since this register is confirmed
  unreliable for Constant Flow specifically, `ControlService` no longer
  applies its readback to the cached setpoint for this mode; the display now
  reflects only the last client-commanded value (from
  `set_constant_flow_async()`), showing unavailable until a value is
  explicitly set rather than a definitely-wrong number. Other modes are
  unaffected — they still read their setpoint from the register as before.
  A diagnostic warning log (`check_flow_setpoint_scale()`, added alongside
  this fix) is kept in case a different pump/firmware revision behaves
  differently.
  (Enabling the pump in Constant Flow mode no longer forces a hardcoded
  ~3671 RPM either, as that shares the fix for #43.)
  (issue [#44](https://github.com/eman/esphome-alpha-hwr/issues/44),
  PR [#48](https://github.com/eman/esphome-alpha-hwr/pull/48)).
- **Pump Enabled ignored the configured setpoint, forcing a hardcoded ~3671 RPM** —
  `ControlService::start()` called `send_control_request()` with no setpoint,
  so `CLASS10_CONTROL_MAP`'s default suffix (which decodes to exactly `3671.0`)
  was always sent for Constant Pressure, Proportional Pressure, Constant Speed,
  and Constant Flow modes on every enable, regardless of any setpoint
  previously configured. `start()` now reuses the pump's cached setpoint
  (converting meters back to Pascals for the pressure modes) when no explicit
  mode override is requested and a setpoint has already been read for the
  current mode; falls back to the previous default-suffix behavior otherwise,
  including for DHW On/Off, Temperature Range, and AutoAdapt modes
  (issue [#43](https://github.com/eman/esphome-alpha-hwr/issues/43),
  PR [#47](https://github.com/eman/esphome-alpha-hwr/pull/47)).
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

[Unreleased]: https://github.com/eman/esphome-alpha-hwr/compare/v0.8.0...HEAD
[0.8.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/eman/esphome-alpha-hwr/releases/tag/v0.1.0
