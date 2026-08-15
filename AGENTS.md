# AGENTS.md - Developer Guide for Agents

This document serves as the **primary reference for AI Agents** and developers working on the `esphome-alpha-hwr` project. It defines the project's goals, coding standards, and strategic approach to ensure consistency and quality.

## 1. Project Mission & Goals

Our mission is to provide a robust, reliable, and feature-rich ESPHome component for the **Grundfos ALPHA HWR** hot water recirculation pump.

### core Objectives

1. **Control Capability**: Implement bi-directional control (Start/Stop, Set Mode, Schedules) via BLE.
2. **Stability**: Ensure the component is stable significantly longer than the transient connection times of a mobile app.
3. **User Experience**: Provide "It Just Works" discovery and configuration for Home Assistant users.

### Strategic Principles

* **Incrementalism**: We build complexity layer-by-layer.
  * *Phase 1:* Passive Telemetry (READ only).
  * *Phase 2:* Authenticated Telemetry (UNLOCK + READ).
  * *Phase 3:* Basic Control (WRITE commands).
  * *Phase 4:* Complex Management (Schedules, Time Sync).
* **Simple Configuration**: The `hwr-pump-example.yaml` configuration should demonstrate best practices and be the reference for users. We prioritize good architecture over backward compatibility since this is a new library.
* **No Regressions**: New features (e.g., adding schedule writing) must not break existing features (e.g., live flow rate reporting).

## 2. References

Agents should consult these resources before making architectural decisions:

* **Protocol Documentation**: [https://eman.github.io/alpha-hwr/reimplementation/](https://eman.github.io/alpha-hwr/reimplementation/) (The "Bible" for the GENI byte-level protocol).
* **ESPHome BLE Client**: [https://esphome.io/components/ble_client/](https://esphome.io/components/ble_client/)
* **ESPHome Bluetooth Proxy**: [https://esphome.io/components/bluetooth_proxy/](https://esphome.io/components/bluetooth_proxy/)

## 3. Development Standards

### C++ (ESPHome Component)

* **Style**: Follow standard ESPHome/Google C++ conventions.
* **Logging**: Use `ESP_LOGx` macros for all output.
  * `ESP_LOGV`: Packet dumps, heavy frequency loop data.
  * `ESP_LOGD`: State transitions, single packet summaries.
  * `ESP_LOGI`: Connection events, successful auth, major status changes.
  * `ESP_LOGW`: Retries, unexpected (but handled) data, timeout warnings.
  * `ESP_LOGE`: Critical failures, unrecoverable errors.
* **Endianness**: The GENI protocol is **Big-Endian**. Always use helper functions (e.g., `read_float_be`, `put_unaligned_be32`) or standard `htonl`/`ntohl` to ensure portability. **Do not assume host endianness.**
* **State Machines**: Explicitly model complex interactions (e.g., Authentication Handshake) as state machines `enum class State { IDLE, AUTH_CHALLENGE, AUTH_RESPONSE, CONNECTED }`. Avoid deep nested `if/else` in the loop.

### Python (Support Scripts/Tools)

* **Formatting**: Use `black` and strict type hinting (`mypy`).
* **Structure**: Keep tools modular. If writing a script to parse logs, put it in `tools/`.

## 4. Testing Strategy

### Unit Testing (Host-Based)

Logic that does not depend on ESP hardware **MUST** be testable on the host machine. This includes:

* **CRC Calculations**: Verify `calc_crc16` against known test vectors.
* **Frame Encoders/Decoders**: Verify that `build_packet` produces the exact byte sequence expected by the pump.
* **Parsing Logic**: Verify `decode_packet` correctly extracts float values from raw hex strings.

*Action*: Create simple C++ test files (e.g., `tests/test_protocol.cpp`) that can be compiled with `g++` or `clang` on the developer's machine to verify this logic without flashing a device. Run the whole suite with `cd tests && make test`.

Two established styles in `tests/`:

* **Logic replicas** (`test_control_state.cpp`) — re-declare minimal copies of production enums/logic in the test file; no component sources compiled.
* **Real sources with mocks** (`test_schedule_service.cpp`, `test_write_operations.cpp`) — compile the actual component `.cpp` files against the mock ESPHome SDK in `tests/mocks/`, capture outgoing frames via `transport.set_write_callback`, drive time with the `mock_millis` global, and inject synthetic response frames via `transport.on_notification`. `test_write_operations.cpp` adds a small pump simulator that answers reads and applies/ignores/clamps writes, which is the template for testing write-operation behavior end-to-end.
* **A whole ESPHome component** (`test_dhw_demand_component.cpp`) — same idea one level up: instantiate the component, wire mock entities through its real setters, call `setup()` and then `update()` on the `mock_millis` clock, and assert on what the output entities received. Use this for the parts of a component that live in its `.cpp` and nowhere else — sensor-callback wiring, branch selection, publish gating.

**A mock must match real ESPHome, not a convenient version of it.** A mock that behaves better than the SDK makes the code guarding against the SDK look like an oversight, and the test then passes against a bug that does not exist. The entity mocks are annotated with the behaviour they mirror and why it matters: `sensor` and `text_sensor` publish unconditionally while `binary_sensor` de-duplicates (which is what `publish_gate.h` exists for), and `has_state()` means "has published at least once", never "is fresh" (which is what the `dhw_demand` staleness registers exist for).

**If a component `.cpp` can be compiled against the mocks, it should be.** A file no host target compiles is a file the mutation check cannot reach, and `esphome compile` catching a type error is a ten-minute CI round trip that says nothing about behaviour. `dhw_demand.cpp` sat in exactly that state, and the cost was visible: a block referencing an out-of-scope variable passed 905 green tests and cppcheck, and only the firmware build objected.

`alpha_hwr.cpp`, `ble_connection_manager.cpp` and `api_bridge.cpp` genuinely need ESP-IDF or the API SDK, and stay firmware-build-only. Everything else in `components/` is host-compiled and host-tested, with one exception: `time_service.cpp`, which needs a `esphome/components/time/real_time_clock.h` mock that does not exist yet.

Two things the first round of this taught, worth repeating when adding the next target:

* **Compile with the same defines the firmware uses.** `sensor_publisher.cpp` keeps the issue #127 publish guards inside `#ifdef USE_TEXT_SENSOR`, and the component's `AUTO_LOAD` defines it on every real build — so the host target passes `-DUSE_TEXT_SENSOR`. Without it the target would compile a *different program* and report guards as covered that had never been compiled.
* **Expect the first host compile to find something.** Adding `device_info_service.cpp` turned up three dead things in one pass that the firmware build had never reported: a file-static `TAG` shadowed by the class's own, a lambda capturing `this` and not using it, and an unused `Session &` member (which the constructor no longer takes).

### Hardware Verification

Before marking a task as complete, verify on actual hardware using your private, gitignored hardware config:

1. **Compile & Flash**: Ensure no compilation errors.
2. **Discovery**: Does the device show up? (Check logs for "Found ALPHA HWR").
3. **Connection**: Does it connect *and stay connected*?
4. **Telemetry**: Do values update? (Wave hand over pump or start water flow to verify changes).

Note: `hwr-pump-example.yaml` is for documentation and compilation testing only (contains placeholder values). Use the private, gitignored config with real device values for actual hardware testing. Never name that file in repo-facing prose — changelog, docs, issues, or PRs.

#### Bench session hygiene (issue #127)

* **At most one extra API subscriber beyond Home Assistant.** Every log line and
  every state publish is an API frame fanned out to *each* subscriber. Stacking
  HA + an `esphome logs` stream + `aioesphomeapi` polling scripts at
  `level: DEBUG` has exhausted the heap in ESPHome's outgoing-frame buffer and
  rebooted the node mid-session.
* **A reboot is not automatically a regression in what you just flashed.** Check
  the decoded backtrace first: `esp32.crash` reports the previous boot's fault,
  and a crash inside `esphome::api::*` with no `alpha_hwr` frames is API buffer
  pressure, not the change under test. Only the most recent boot's crash record
  is readable, so decode it before reflashing.
* **Watch Free Heap / Min Free Heap / Heap Fragmentation** (exposed by the
  packages via the `debug` component) when a session runs long, and check
  "Reset Reason" after any unexplained restart.
* **Don't add periodically-publishing entities without a change gate.**
  ESPHome's `number`/`select`/`sensor`/`text_sensor` `publish_state()` do *not*
  dedup (only `binary_sensor` and `switch` do), so a polled template lambda or a
  per-tick component publish emits a frame per subscriber per interval even when
  nothing moved. Use `publish_number_if_changed()` /
  `publish_option_if_changed()` (`components/alpha_hwr/publish_gate.h`) in
  polled lambdas, `publish_sensor_if_changed()` /
  `publish_text_sensor_if_changed()` (`components/dhw_demand/publish_gate.h`) for
  a component's own `sensor`/`text_sensor` outputs, and fire C++ state callbacks
  only on actual transitions. A consumer that genuinely needs the repeats asks
  for them with `force_update: true` on the sensor, which the gate honours.

## 5. Documentation Requirements

* **Code Comments**: Explain *why* a specific byte sequence is used (reference the protocol doc).
  * *Bad*: `// Send 0x02`
  * *Good*: `// Send 0x02 (Class 10 Start Byte) - See Protocol Doc Sec 3.1`
* **PR/Commit Messages**: Clearly state what changed and what was tested.
* **README updates**: If a new feature is added (e.g., a "Boost Mode" switch), update the `README.md` and `hwr-pump-example.yaml` Config section immediately.

## 6. Architecture: Layered Service-Based Design

The ESPHome component follows a layered, service-based architecture. This architecture separates concerns, improves testability, and makes the codebase maintainable.


**Key Architectural Principles:**

1. **Separation of Concerns**: Each layer has a single, well-defined responsibility.
2. **Client as Facade**: The `AlphaHwrComponent` is a thin orchestration layer that delegates all work to services.
3. **Services Own Business Logic**: Each service encapsulates all operations for its domain (e.g., `TelemetryService` handles all telemetry operations).
4. **Protocol Layer is Stateless**: Frame builders and parsers are pure functions with no side effects.
5. **Core Layer Manages State**: Transport handles BLE I/O, Session tracks connection state, Authentication handles handshake.
6. **One Write Path**: Every pump write — entity-originated or service-originated — goes through the write-operation layer (`services::WriteOperationService`, issue #92), which serializes write sequences, confirms each write against a pump readback, and reports exactly one terminal settle result per operation.

### ESPHome Component Implementation (COMPLETED)

> **Note on Structure**: ESPHome requires a flat file structure in `components/alpha_hwr/`, so the layered architecture is implemented using **C++ namespaces** instead of subdirectories. This provides the same logical separation while maintaining ESPHome compatibility.

**Current Structure:**

```
components/alpha_hwr/
├── alpha_hwr.h/cpp                  # Main component (thin facade, orchestration)
├── api_bridge.h/cpp                 # HA services + write_settled event (issue #92)
├── core::                           # Foundation layer (namespace)
│   ├── transport.h/cpp              # BLE I/O, command queue, FSM transaction manager
│   ├── session.h/cpp                # Connection state machine
│   ├── auth.h/cpp                   # Authentication handshake
│   └── ble_connection_manager.h/cpp # BLE connection lifecycle management
├── protocol::                       # Protocol layer (namespace, stateless)
│   ├── codec.h/cpp                  # Endianness-safe encoding/decoding, CRC
│   ├── frame_builder.h/cpp          # Build GENI request packets
│   ├── frame_parser.h/cpp           # Parse GENI responses, validate CRC
│   └── telemetry_decoder.h/cpp      # Decode Class 10 DataObjects to telemetry
└── services::                       # Business logic layer (namespace)
    ├── write_operation_service.h/cpp # Write lifecycle: serialize, confirm, settle events
    ├── telemetry_service.h/cpp      # Sensor readings and polling
    ├── control_service.h/cpp        # Pump-state cache + control wire primitives
    ├── schedule_service.h/cpp       # Weekly schedule management
    ├── device_info_service.h/cpp    # Device ID strings + operating statistics
    ├── time_service.h/cpp           # Pump RTC synchronization
    ├── event_log_service.h/cpp      # Start/stop event history
    ├── history_service.h/cpp        # Trend data + cycle timestamps
    └── sensor_publisher.h/cpp       # Map telemetry to ESPHome sensors
```

**Implementation Highlights:**

* **Namespace Organization**: `esphome::alpha_hwr::core`, `esphome::alpha_hwr::protocol`, `esphome::alpha_hwr::services`
* **Thin Facade**: `AlphaHwrComponent` delegates all operations to services (no direct protocol manipulation)
* **Non-Blocking Transport**: Command queue + FSM state machine (`IDLE`, `SENDING_CHUNKS`, `AWAITING_RESPONSE`)
* **Transaction Safety**: 50ms pacing between commands, response matching by Object/Sub-ID
* **Write-and-Verify**: One write path through the operation layer; every write is confirmed against a pump readback and reported with a terminal settle event (see §8.4 and `docs/programmatic-interface.md`)

### Benefits of This Architecture

1. **Testability**: Protocol layer can be unit tested without hardware.
2. **Maintainability**: Each file has a clear, single responsibility (avg ~200-300 lines vs original 1300+ line monolith).
3. **Extensibility**: New features (e.g., `HistoryService`, `TimeService`) can be added without touching existing code.
4. **Debuggability**: Smaller files and clear boundaries make bugs easier to isolate.
5. **ESPHome Compatibility**: Flat file structure with namespace-based organization meets ESPHome requirements.

### Rules for Future Development

1. **Maintain Layering**: New features should be added to the appropriate layer/namespace.
2. **Document Protocol References**: Every packet builder/parser must cite the protocol doc section.
3. **Test After Changes**: Verify `hwr-pump-example.yaml` compiles and the private hardware config works on hardware.
4. **Keep Services Focused**: Each service should own a single domain (telemetry, control, schedules, etc.).
5. **No New Standalone Write Paths**: Anything that writes to the pump must be a `WriteCommand` in the operation layer (see §9), so it inherits serialization, confirm readbacks, and the one-terminal-event contract.

## 7. Current Status & Implementation Progress

### Completed Features

#### Phase 1-2: Telemetry & Authentication (COMPLETE)

* [x] BLE discovery and connection
* [x] Authentication handshake (challenge/response)
* [x] Live telemetry streaming (flow rate, pressure, power, temperature)
* [x] All sensors exposed to Home Assistant
* [x] Connection stability and reconnection logic

#### Phase 3: Basic Control (COMPLETE)

* [x] Start/Stop pump commands
* [x] Mode selection (Auto, Manual, Off)
* [x] Setpoint adjustments (temperature targets)
* [x] Control buttons in Home Assistant

#### Phase 4: Schedule Management (COMPLETE)

* [x] Schedule reading from all 5 layers (0-4)
* [x] Schedule parsing and decoding
* [x] Schedule display in Home Assistant
* [x] Schedule packet building (53-byte APDU format)
* [x] Schedule validation logic
* [x] **Schedule write persistence** (RESOLVED via Non-Blocking Transaction Manager)

Additional services beyond the original phases: `device_info_service` (device
ID strings + statistics), `time_service` (RTC sync), `event_log_service`,
`history_service`.

#### Phase 5: Programmatic Write-and-Verify Interface (COMPLETE — issue #92)

* [x] `WriteOperationService`: one serialized write path with confirm readbacks and terminal statuses (accepted/clamped/rejected/timeout/superseded)
* [x] Entity writes routed through the operation layer (anti-clobber for the dashboard too)
* [x] HA services (`set_pump_enabled`, `set_mode`, `set_setpoint`, `set_temperature_range`, `set_cycle_times`) registered in C++ (`api_bridge`)
* [x] Schedule services migrated from YAML to `api_bridge` with verify readbacks (previously they reported success unconditionally)
* [x] `esphome.alpha_hwr_write_settled` event: exactly one terminal event per write, self-identifying via caller-supplied `op_id`
* [x] Host test suite `tests/test_write_operations.cpp` (pump simulator driving every terminal status)

See `docs/programmatic-interface.md` for the public contract.

## 8. The Solution: Non-Blocking BLE Transaction Manager

### 8.1 The Challenge
The Grundfos pump uses a **two-phase commit** protocol for many operations (especially flash writes like schedules). The client must:
1. Send the write packet.
2. **Wait for and actively consume** an acknowledgment notification (usually Type 0xDE01).
3. Failing to consume the ACK within a specific window causes the pump to discard the change.

In ESPHome's single-threaded environment, a simple `delay()` or `while(!available())` freezes the device. An async approach that just registers a callback and returns immediately often misses the ACK because the event loop continues and other operations (like telemetry polls) interfere.

### 8.2 The Implementation: Command Queue + FSM
The refactored `core::Transport` layer implements a **Finite State Machine (FSM)** and a **Command Queue** (`std::deque<Command>`) to manage transactions safely.

#### The State Machine
* **IDLE**: Waiting for a new command in the queue.
* **SENDING_CHUNKS**: Splitting large packets (like the 53-byte schedule) into 20-byte BLE MTU chunks with 50ms pacing.
* **AWAITING_RESPONSE**: Non-blockingly waiting for a matching notification based on Object ID and Sub-ID.

#### Pacing & Isolation
The `Transport::loop()` (called every iteration of `AlphaHwrComponent::loop()`) ensures that:
1. Only one command is "in flight" at a time (Transaction Isolation).
2. A minimum of 50ms exists between every BLE write (Pacing).
3. Response matching is performed against incoming notifications before any new command is sent.

### 8.3 Firmware Quirks Handled
The implementation includes specific logic for pump-specific behaviors:
* **SubID 0 Quirk**: The pump often responds with `SubID 0` even when a specific Sub-ID (like `1000`) was requested. The matching logic now treats `SubID 0` as a wildcard match for the requested Object ID.
* **OpSpec 0x01**: Some writes trigger an immediate `OpSpec 0x01` response. The matching logic has been tuned to be flexible while ensuring the transaction window remains open long enough for the commit to finish.

### 8.4 The Write-Operation Layer (issue #92)

The transport FSM serializes individual *commands*; `services::WriteOperationService` serializes *write sequences* on top of it. A pump write is usually several wire steps (a mode change, a register write, a configuration commit, a confirm readback) — on the legacy paths those steps from different writes could interleave, folding stale cached values into fused frames (the root cause behind #43/#45/#52/#83/#91/#97).

The operation layer's rules:

1. **One operation in flight** — later submissions queue; a newly submitted write SUPERSEDES a still-queued write to the same resource (last write wins), but never aborts an operation mid-wire.
2. **Explicit arguments, not cache reconstruction** — each operation builds its frames from the values passed in. The only cache reads are deliberate (e.g. reusing a mode's stored setpoint for start/stop, with the NaN "keep existing" sentinel when cold).
3. **Confirm readbacks decide the verdict** — a write settles as `accepted`/`clamped`/`rejected` based on what the pump reports actually holding, not on the ACK (the two-phase-commit window of §8.1 often closes without a matchable ACK even on success).
4. **Exactly one terminal event per operation** — validation failures reject before any wire write, a per-operation watchdog converts stuck operations into `timeout`, and a BLE disconnect terminal-events everything pending. A client waiting on `esphome.alpha_hwr_write_settled` can never hang.

`ControlService` retains the pump-state cache, the issue-#91 commanded-but-unconfirmed guards, and the wire primitives; `ScheduleService` likewise. The operation layer composes those primitives (it is a `friend` of `ControlService`) and owns all sequencing.

## 9. Workflows

### Creating New Features

The layered architecture is now in place. When adding new features:

1. **Check Protocol Doc**: Consult the [GENI protocol documentation](https://eman.github.io/alpha-hwr/reimplementation/) for the packet formats involved.
2. **Identify Layer**: Determine which namespace/layer owns this feature:
   * `protocol::` for packet encoding/decoding (stateless)
   * `core::` for BLE transport, session state, authentication
   * `services::` for business logic (telemetry, control, schedules, etc.)
3. **Plan**: Define the packet structure and state flow.
4. **Implement Protocol**: Add packet builder/parser functions in the `protocol` namespace (codec, frame_builder, frame_parser, telemetry_decoder).
5. **Unit Test**: Verify the packet builder produces the correct hex against captured byte sequences.
6. **Implement Service**: Add business logic to the appropriate service in the `services` namespace or create a new service.
7. **Integration**: Hook the service into the main `AlphaHwrComponent` class (add accessors as needed).
8. **Verify**: Compile `hwr-pump-example.yaml`, flash the private hardware config, and test on hardware.

### Adding a New Write Operation (issue #92 contract)

Any feature that WRITES to the pump must be a `WriteCommand` in
`services::WriteOperationService` — never a standalone write path:

1. **Define the command**: add it to `WriteCommand`, the `Operation` fields, and `write_command_to_string()`. That string is public API: it is the event's `command` field, and — if the command gets a service in step 5 — also the name `api_bridge` registers it under, since registration derives the name from the command (#159). An entity-only command like `SET_REMOTE_MODE` is event-surface only. Either way, pin it in `test_command_strings()`, which fails until you do.
2. **Wire steps**: implement `run_<command>_()` composing wire primitives from `ControlService`/`ScheduleService` (add a primitive there if the write needs a new APDU; keep it side-effect-free beyond the wire write).
3. **Confirm comparator**: implement `confirm_<command>_()` — read the value back from the pump and decide accepted/clamped/rejected; overwrite the operation's fields with the SETTLED values before finishing so the event reports what the pump holds.
4. **Resource key + budget**: add a supersede key in `resource_keys_()` and a watchdog budget in `start_front_()`.
5. **Surface it**: add a `submit_*` method, a facade passthrough in `alpha_hwr.h`, a service registration + event fields in `api_bridge.cpp`, and (if entity-driven) route the entity lambda through the facade.
6. **Host test**: add cases to `tests/test_write_operations.cpp` covering at minimum: accepted, one failure status, and the one-terminal-event invariant.
7. **Document**: add the service + event fields to `docs/programmatic-interface.md`.


---

## 10. Release Process

### Versioning

Releases follow **semantic versioning** (`vMAJOR.MINOR.PATCH`). Because this library is still pre-1.0, minor version bumps (`v0.x.0`) are used for new features or breaking changes; patch bumps (`v0.x.y`) are used for bug fixes only.

### Creating a Release

1. **Merge all PRs** for the release into `main`.
2. **Tag and publish** via the GitHub CLI:
   ```bash
   gh release create vX.Y.Z \
     --title "vX.Y.Z — Short description" \
     --notes-file /tmp/release_notes.md
   ```
3. **Update all version pins** in the repository (see below) and commit directly to `main`:
   ```bash
   # Bulk-replace the previous tag across all example YAMLs and packages
   old=vOLD; new=vX.Y.Z
   sed -i '' "s|@${old}|@${new}|g" \
     hwr-pump-example.yaml hwr-pairing-example.yaml \
     hwr-pump-schedule-example.yaml dhw-demand-example.yaml \
     packages/alpha_hwr_base.yaml packages/alpha_hwr_pairing.yaml \
     packages/dhw_demand_detector.yaml
   git add -u && git commit -m "Pin examples and packages to ${new}"
   git push
   ```

### Files That Must Be Updated on Every Release

> **Rule**: whenever a new release tag is created, every `@vX.Y.Z` ref in the files below must be updated to the new tag. Failing to do so means users who copy-paste the examples will pull an outdated version.

| File | Role |
|---|---|
| `hwr-pump-example.yaml` | Basic pump example |
| `hwr-pairing-example.yaml` | Pairing example |
| `hwr-pump-schedule-example.yaml` | Schedule management example |
| `dhw-demand-example.yaml` | DHW demand detector example |
| `packages/alpha_hwr_base.yaml` | `external_components` source for base package |
| `packages/alpha_hwr_pairing.yaml` | `external_components` source for pairing package |
| `packages/dhw_demand_detector.yaml` | `external_components` source for DHW demand package (including commented examples) |

Do **not** update files under `.esphome/` — that directory is a local build cache and is not committed.

---

## 11. Component: `dhw_demand` — DHW Demand Detector

### 11.1 Background & Motivation

The `components/dhw_demand` component addresses a fundamental ambiguity in hot-water recirculation systems: **a flow sensor in the DHW circuit cannot distinguish closed-loop recirculation from an occupant actually opening a fixture**. The household flow sensor sits inline in the DHW circuit and reports flow regardless of source — when the ALPHA HWR pump is running a nonzero reading may be recirculation only, demand only, or both simultaneously. The ALPHA HWR's internal flow sensor (0–0.53 GPM) is blind to demand when the pump is idle.

The theoretical foundation is fully documented in the companion research project:

* **`docs/hot-water-research.md`** — exhaustive treatment of hydrodynamic and thermodynamic signatures that separate recirculation from genuine DHW demand. Key insight: the instant an occupant opens a valve the system topology changes from **closed-loop** to **open-loop**, producing measurable hydraulic transients (pressure drop, flow-rate collapse at the pump, current/power spike) that cannot be explained by normal recirculation.
* **`docs/esp32-detector.md`** — describes how the detection algorithm is ported to an ESP32/ESPHome environment, including sensor requirements, memory footprint, threshold defaults, and the MQTT output format.

**Design philosophy:** We take an explicit, on-device approach — no ML, no model training, no InfluxDB dependency. On the pump-off branch each physical signal votes independently and confidence comes from vote weight and count. On the pump-on branch there are no votes: household demand is *measured* as `flow − pump_flow` (issue #149), because controlled measurement showed the hydraulic votes could not be made correct at any threshold. Prefer measuring a quantity over voting on proxies for it; where a threshold is unavoidable, it must be placed from data and the data recorded next to it.

### 11.2 Architecture

`DhwDemandComponent` is a standalone `PollingComponent` (default 10 s tick) in namespace `esphome::dhw_demand`. It has **no dependency on `alpha_hwr`** — pump telemetry sensors are wired in by the YAML config, so the component works whether the pump sensors come from `alpha_hwr` or any other source.

```
components/dhw_demand/
├── __init__.py       # ESPHome config schema; all inputs/outputs/thresholds optional
├── dhw_demand.h      # DhwDemandComponent class definition
└── dhw_demand.cpp    # Detection logic, session tracking, publish helpers
```

### 11.3 Sensor Inputs

All inputs are optional — missing sensors produce `NAN` and the affected detection paths are simply skipped.

#### Pump telemetry (sourced from `alpha_hwr` sensors)

| Config key | Signal | Notes |
|---|---|---|
| `motor_speed` | RPM | Primary pump-state indicator; also gates the pump-on subtraction below `pump_on_demand_min_speed_rpm` |
| `motor_current` | A | Fallback pump-state indicator when RPM is absent |
| `pump_flow` | GPM | The pump's own recirculation-loop reading, subtracted from `flow` to measure household demand while the pump runs (reads 0 when the pump is off, so the same expression collapses to the pump-off rule) |

`inlet_pressure`, `pump_power` and `pump_head_rate` were removed in issue #149 with the vote tier that read them. The keys are still in the schema, mapped to a validator that fails with an explanation.

#### Supplementary sensors (fetched from Home Assistant via `platform: homeassistant`)

| Config key | Signal | Notes |
|---|---|---|
| `flow` | GPM | Household inline DHW circuit meter; detects **both** recirculation and demand flow, so it is only unambiguous when the pump is off — while the pump runs it is one term of the subtraction, never a threshold in its own right; **30-sample circular buffer** (5 min at 10 s grid) |
| `tank_lower_temp` | °F | Tank thermal collapse derivative |
| `dhw_charge` | % | DHW charge-drop derivative |
| `dhw_in_use` | boolean (as float) | Heater's native DHW flag. Boosts confidence by +0.05 when the pump is off, and — guarded by `dhw_in_use_min_seconds` — is the last-resort pump-on recall tier |

### 11.4 Detection Algorithm

The tick runs in `update()` every 10 seconds. Steps:

1. **Read sensors & compute derivatives** — `Δx/Δt` using actual elapsed ms so jitter in the update interval doesn't bias rates. Only the two pump-off thermal signals need rates now; the pressure/current/power derivatives went with the vote tier they fed (#149).
2. **Push household flow into 30-sample circular buffer** — used by the falling-edge latch.
3. **Determine pump state** — `motor_speed > 0` preferred; `motor_current ≥ pump_off_current_threshold` fallback.
4. **Run the appropriate detection branch** (pump-off or pump-on).
5. **DHW-in-use confidence boost** — +0.05 if the heater's `dhw_in_use` flag corroborates demand.
6. **Publish results** and **update session tracking**.

#### Pump-OFF branch

When the pump is off the household flow sensor reads only genuine demand flow, making it the unambiguous ground-truth signal. Three signals vote independently:

| Signal | Condition | Weight |
|---|---|---|
| Household flow | `flow > flow_threshold` (0.3 GPM) | 1.0 |
| Thermal collapse | `Δtemp/Δt < −thermal_collapse_rate` (0.05 °F/s) | 0.9 |
| Charge drop | `Δcharge/Δt < −dhw_charge_drop_rate` (0.005 %/s) AND tank not warming | 0.7 |

Confidence = highest-weight signal + 0.05 per additional corroborating signal, capped at 1.0.

**No-flow guard:** if current household flow is below threshold *and* the 30-second falling-edge latch has expired, demand is suppressed regardless of other signals. This is the primary false-positive filter.

> **The 0.3 GPM floor stays, and it is now measured rather than assumed (#180).** The floor does discard sub-threshold readings, and the readings are real signal — the meter reports a hard `0.0` at rest rather than dithering. What the measurement changed is what those readings turn out to *be*.
>
> Over 10.4 days of one installation (2,646 meter samples, 1,306 with the pump confirmed off, 81 pump-off edges), 160 pump-off samples fell in the 0.05–0.30 band. Splitting them by proximity to a pump-off edge — the analysis this note used to ask for:
>
> - **Shutdown decay is real and measurable.** 33.1 % of the band sits within 30 s of a pump-off edge against a 10.3 % base rate for pump-off samples generally — 3.2× enriched, so ~23 % of the band is the pump's own collapsing loop flow. The bands *above* the floor are enriched at 0.71× and 0.49×, i.e. depleted near edges. Decay lands in the sub-threshold band specifically, which is exactly what the 0.3 floor is keeping out.
> - **The genuine remainder is almost entirely already detected.** Of the sub-threshold samples clear of the decay window, **89 % lie within 60 s of a reading that already exceeds 0.30** (97 % within 5 minutes). They are the ramp-up and tail-off shoulders of draws the detector has already declared, not draws it missed. A session is already open when they arrive.
> - **Truly isolated small draws: one episode in 10.4 days** — 136 s at 0.14–0.17 GPM, with no reading above 0.30 within ±5 minutes. That is the entire recoverable population, ~0.1/day.
> - **Onset latency does not improve either.** Requiring genuinely contiguous samples (≤30 s apart) the median lead a 0.05 floor would buy on draws that do cross 0.30 is **0 s**. An earlier draft of this analysis reported a 288 s median; that number was an artifact of allowing 120 s gaps in the run-up walk-back, which chains unrelated readings into a "ramp" that never happened.
>
> So lowering the floor would admit a measurable decay population to recover roughly one short draw per ten days, and buy no earlier detection. The 14 h sample that opened #180 counted *samples* and read 24 sub-threshold pump-off readings as discarded demand; at episode level over 18× the window, they are overwhelmingly the shoulders of draws already caught. **The floor stays at 0.3 — now on evidence.** Do not lower it on intuition, and note that a sample-count argument is the specific way this one looked wrong.

**Falling-edge latch:** if household flow was above threshold within the last `flow_latch_seconds` (30 s) but has since dropped (burst-cadence gap), demand is held alive to prevent a single missed reading from causing a false termination.

**The latch is disarmed for `latch_pump_off_suppression_seconds` (30 s) after a pump-off edge.** A shutdown presents the same shape as a burst-cadence gap for a different reason — loop flow runs a median 1.45 GPM and collapses through the threshold within seconds of the motor parking — so inside that window the latch would be standing on the pump's own flow. Because the flow signal outranks thermal and charge on confidence, a *published* thermal or charge verdict means flow was below threshold now and above it within the latch window, which is exactly what a shutdown supplies. Measured on the Python detector over 30 days, 71 % of thermal and 62 % of charge firings fell within 30 s of a pump-off edge against 0.38 % of pump-off cells; suppressing the latch removed 93 false positives for 1 true positive. A real draw puts the meter above threshold on its own and never consults the latch, which is why this costs almost no recall.

#### Pump-ON branch

When the pump is running the household flow meter sees recirculation flow in addition to any demand, so it cannot be read directly. The ordering below is `decide_pump_on()` in `dhw_demand_logic.h`, not inline in `update()` — it is a pure function so the host test can assert tier priority, which it could not while the composition lived in the `.cpp` (issue #144).

1. **Continuation detection** — if household flow was above threshold on the last pump-off tick, the draw is presumed still running at confidence 0.85 until something ends it. This handles draws already in progress when the pump turned on.

   **Releasing it is the hard part, and the spec used to have a hole here.** The only exit was "and household flow is still above threshold now", a raw-meter test that cannot go false while the pump runs: every pump-on meter reading recorded here — 0.71 GPM at the 1650 rpm clamp floor, 1.31 no-draw median, 1.45 pre-shutdown, 2.22 no-draw p90 — clears the 0.3 threshold by more than 2×. So a draw that stopped five minutes into a thirty-minute run went on publishing demand for the remaining twenty-five, with the subtraction sitting there reading 0.00 GPM. The no-draw distribution quoted in the warning below is itself the proof: if no threshold on raw meter flow can *detect* a draw, none can detect one *ending* either. Three exits now, and the raw-flow test survives only for what it can still decide (a dead or genuinely zero meter):

   | Exit | When | Effect |
   |---|---|---|
   | Measured stop | The subtraction is available (all of tier 2's own guards) and reads **at or below zero** on `continuation_release_ticks` (2) consecutive ticks | Releases **and retires the capture** — a measurement falsifies the claim, so a later loss of the subtraction must not resurrect it |
   | Expiry | No measurement has *supported* the draw for `pump_on_continuation_max_seconds` (300). A subtraction clearing the firing threshold re-stamps the support time, so a continuously measured draw never expires | Releases **and retires the capture** — not because the age could fall again on its own, but because the age is an unsigned difference and wraps through zero ~49 days after arming |
   | Meter quiet | Household flow is NaN or at/below threshold | Releases, capture kept — one dropped sample must not permanently end a continuation that is still true |

   **The release threshold is zero, not `pump_on_demand_flow_threshold`, and the asymmetry is the point.** 0.3 GPM is where the subtraction is confident enough to *raise* a claim. Borrowing it to *falsify* one inverts the safety argument the subtraction rests on: the loop estimate's residual is negative (−0.10 ± 0.06 GPM steady, −0.14 at 2398 rpm), which is conservative while it can only suppress a claim and becomes a false-negative generator the moment it can retire one. A genuine draw just above the arming gate computes below 0.3 and was read as "stopped", killing the tier for the rest of the run with the tap still open — measured on hardware at +0.226 GPM, held for 26 s with water running. The band is bounded on both sides: under 0.3 GPM a draw never arms the tier, and over 0.3 − residual the old comparison held anyway, so steadily it is roughly 0.3–0.4 GPM and widens only during the transients the tick count covers. Zero is the defensible line — the meter reading no more than the pump says its own loop is moving is the only unambiguous statement that no household draw exists; any positive difference is evidence of *something*, and the expiry handles what this cannot decide.

   **Two consecutive ticks, because the retirement cannot be undone within a pump run.** The channels do not report together — the meter led the pump by 13 s at one recorded start — so a mid-run speed change lets the meter follow the loop down while the pump's own last report is still high and inside its 30 s freshness bound, and the difference goes sharply negative for a tick or two with the tap still open. The −0.47 GPM logged on the bench that verified this tier is itself such a transient: recorded while the pump wound down from 3667 to 2499 rpm, 3–5× the steady residual.

   The expiry is not belt-and-braces: the subtraction goes silent below `pump_on_demand_min_speed_rpm`, so on a pump clamped under that floor no measurement exists to support or contradict the claim and it would otherwise stand for the whole run. `0` disables the tier rather than unbounding it.

   Note what this is not: releasing on the subtraction is tier 2's own oracle under tier 2's own guards. It adds no signal and does not touch the raw-flow prohibition below — indeed the same paragraph that states the prohibition prescribes the subtraction as its remedy, so reading "keys off raw meter flow" as "any expression containing meter flow" would outlaw tier 2 itself. The decision, including which exit fired, lives in `pump_on_continuation_verdict()`; the component acts on the verdict rather than re-deriving it from `detection_method`.

2. **Subtraction** — `flow − pump_flow` is household demand directly: the meter reads everything leaving the mains, the pump reports its own loop, and the difference is what the house drew. No RPM term, no fitted curve. Fires above `pump_on_demand_flow_threshold` (0.3 GPM of *computed* demand); confidence is `min(0.90, 0.60 + 0.30 × margin)`, rising with how far the draw clears the threshold. Three guards carry it, all load-bearing:

   | Guard | Value | Why |
   |---|---|---|
   | Both channels present | — | A difference needs two terms |
   | Both readings current | 30 s pump / 60 s meter | The channels do not report alike: the pump every 10 s, the meter on change at a median 28 s. Gaps in the pump channel beyond 20 s are 1 % of gaps but **14 % of pump running time** |
   | Minimum pump speed | `pump_on_demand_min_speed_rpm`, 1950 | The pump *estimates* loop flow rather than metering it; below ~2000 RPM the estimate reads low and the difference goes spuriously positive with the tap shut (+0.45 GPM measured at 1650) |
   | Loop reading postdates the pump start | — | Fresh-by-age is not enough at a startup: the two channels do not begin reporting together |
   | Loop estimate has settled | `pump_on_demand_settle_seconds`, 10 | The pump can report *promptly* and still be wrong: the impeller is accelerating and the estimate reads low |

   That fourth guard is the newest and the least obvious. At a pump start the meter reports the loop **before** the pump reports its own flow — measured at 13 s on the Python side — so for those seconds the difference is taken against a **stale zero from before the motor started**, comfortably inside the 30 s staleness bound and past the speed floor. `1.43 − 0.00` published as `demand_level` 0.57 at confidence **0.90**, the top of this tier's range. Over 30 days of Python's live output, 42 % of all subtraction firings fell within 10 s of a pump-on edge, against 8.8 % of pump-on cells overall. `reading_predates_pump_start` compares *ages* rather than absolute stamps so it wraps correctly at the `millis()` rollover, and it abstains when no pump start has been observed. It is a regime test, **not** a suppression window: nothing is blocked for a fixed time and the tier resumes on the pump's very next reading. The fixed window that used to sit here (`PUMP_STARTUP_TRANSIENT_SUPPRESSION_MS` with `pump_on_started_ms_`) was retired with the vote tier it gated and is deliberately not reinstated in that shape.

3. **`dhw_in_use` recall tier** — the heater's own DHW in-use flag, once it has been *continuously* high for `dhw_in_use_min_seconds` (70), declares demand at confidence 0.6. The flag is unusable bare — ~77 events/day, median 15 s, of which 70 s clears 89.7 % — and the guard is the whole tier. NaN breaks the run exactly as a low sample does, so a BLE dropout resets the timer rather than holding the last value; this mirrors Python, where a missing sample in the rolling window is a break. Strictly additive: it sits below everything, cannot displace a stronger tier, and only ever adds demand. Intensity comes from the subtraction where available, else the shared no-claim constant 0.4 — never from raw meter flow (issue #143). The tracker ticks in **both** pump branches, because the run has to be free to start while the pump is still off.

4. **Fallback** — `pump_on_uncertain`, `demand=false` at 0.5 confidence. Reached when everything above declined.

> **This replaced a five-signal hydraulic vote tier (issue #149), and that is not to be undone.** The votes thresholded inlet pressure, pump-side flow collapse, and current/power/head derivatives. Controlled measurement showed they could not be made correct: the two absolute votes were scalars on quantities that move with pump speed, so the quiet case can sit 6 PSI *below* the drawing case; and 73 % of the derivative votes' production firings over 29 days fell within 25 s of a self-initiated pump speed change — they were reading the pump's own modulation. Scored on the same 209 cells: votes precision 0.530 / recall 0.936 with 23 pump-on false positives, subtraction 0.808 / 0.894 with **0**.

> **No pump-on rule may key off raw meter flow, ever.** Pump-on runs with no draw read a median 1.31 GPM (p90 2.22) against 1.74 (p90 2.27) with a draw — the distributions overlap almost entirely, so no threshold exists. The "~2.2 GPM recirculation baseline" quoted for years was the p90 of the no-draw case, not a baseline. This is why tier 2 subtracts, and why tier 1 gates on a draw already *confirmed while the pump was off* rather than on a fresh meter reading.

> **Why thermal/duration paths are disabled during pump-on:** During long recirculation runs the pump returns progressively cooled water to the tank cold inlet, causing the lower tank temperature to drop at rates up to −0.083 °F/s — indistinguishable from a shower draw.

### 11.5 Outputs

| Config key | Type | Description |
|---|---|---|
| `demand` | `binary_sensor` | `ON` when DHW demand is detected |
| `confidence` | `sensor` (0–1) | Confidence of current detection result |
| `session_duration` | `sensor` (seconds) | Elapsed seconds of current demand session; 0 when idle |
| `detection_method` | `text_sensor` | Which detection path fired (e.g., `deterministic_flow`, `deterministic_pump_on`, `deterministic_continuation`, `pump_on_uncertain`, `deterministic_idle`) |

### 11.6 Session Tracking

A *session* is a contiguous block of demand ticks. Short gaps (default 60 seconds, `session_gap_tolerance_seconds`) are bridged to avoid fragmenting a single draw into multiple sessions. The session start and end are logged at `ESP_LOGI`.

### 11.7 Tunable Thresholds

All thresholds are exposed as YAML config keys with defaults matching the Python `DetectorConfig`. They can be overridden via `substitutions` without reflashing.

| Key | Default | Unit | Purpose |
|---|---|---|---|
| `pump_off_current_threshold` | 0.03 | A | Motor current below this → pump off |
| `flow_threshold` | 0.3 | GPM | Minimum flow to count as demand |
| `thermal_collapse_rate` | 0.05 | °F/s | Min tank temp drop rate (pump off) |
| `dhw_charge_drop_rate` | 0.005 | %/s | Min DHW charge drop rate |
| `pump_on_demand_flow_threshold` | 0.3 | GPM | Computed demand (`flow − pump_flow`) above which a pump-on draw is declared |
| `pump_on_demand_min_speed_rpm` | 1950 | RPM | Below this the pump's own loop-flow estimate reads low, so the difference goes spuriously positive with no draw |
| `pump_on_demand_max_stale_seconds` | 30 | s | How old the `pump_flow` reading may be and still be differenced |
| `flow_max_stale_seconds` | 60 | s | The same bound for `flow`; looser because the meter reports on change |
| `dhw_in_use_min_seconds` | 70 | s | How long the heater's DHW flag must hold continuously before it may declare a pump-on draw alone |
| `flow_latch_seconds` | 30 | s | Falling-edge hold-off duration |
| `latch_pump_off_suppression_seconds` | 30 | s | Disarms the latch for this long after a pump-off edge, so it cannot be armed by the pump's own collapsing loop flow |
| `pump_on_demand_settle_seconds` | 10 | s | The subtraction declines this long after a pump start, while the pump's own loop-flow estimate is still ramping |
| `pump_on_continuation_max_seconds` | 300 | s | How long a continuation may go unsupported by any measurement before it is retired. A subtraction clearing the firing threshold re-stamps the support time, so this bounds the blind case only; `0` disables the tier |
| `session_gap_tolerance_seconds` | 60 | s | Max gap before ending a session |

### 11.8 Development Rules for `dhw_demand`

1. **No ML, no external dependencies** — the component must compile and run entirely on-device with no Python runtime, no InfluxDB, no trained model. All logic is explicit threshold-based heuristics.
2. **All inputs optional** — never `assert` or crash on a missing sensor. Missing signals return `NAN`; detection paths that require a `NAN` input are silently skipped.
3. **Threshold changes are config changes, not code changes** — if a threshold needs tuning, adjust via YAML `substitutions`, not by editing `.cpp`.
4. **Derivatives use actual elapsed time** — always divide by the real `dt_s` from `millis()` delta, not an assumed 10-second interval.
5. **Consult reference docs before changing thresholds** — `esp32-detector.md` explains the physical rationale for every default. Changes should be grounded in observed hardware behaviour, and the grounding recorded next to the constant. A threshold whose provenance is not written down cannot be defended when it later disagrees with production; that is how the stale `3.0f` head-rate value survived the units audit (#120).
8. **The pump-on tier ordering lives in `dhw_demand_logic.h`, not `update()`** — `decide_pump_on()` is a pure function so the host test asserts tier priority directly (#144). Adding, removing or reordering a tier means changing that function and its ordering tests, never adding a branch in the `.cpp`.
6. **Test compilation** — `dhw_demand` has no BLE dependency; verify it compiles by including it in `hwr-pump-example.yaml` or a minimal test YAML.
7. **Logging discipline** — follow the same `ESP_LOGx` conventions as `alpha_hwr`: `LOGV` for per-tick data, `LOGD` for state transitions, `LOGI` for session start/end. One documented exception: the two continuation retirements log at `LOGI`. They are once-per-continuation by construction, and the default level is `INFO` — which is precisely the configuration in which a field report of "demand stayed on" would otherwise be undiagnosable, since they are the only record of *why* the tier stopped.
