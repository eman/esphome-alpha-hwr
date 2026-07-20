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

### Hardware Verification

Before marking a task as complete, verify on actual hardware using `hwr-pump.yaml`:

1. **Compile & Flash**: Ensure no compilation errors.
2. **Discovery**: Does the device show up? (Check logs for "Found ALPHA HWR").
3. **Connection**: Does it connect *and stay connected*?
4. **Telemetry**: Do values update? (Wave hand over pump or start water flow to verify changes).

Note: `hwr-pump-example.yaml` is for documentation and compilation testing only (contains placeholder values). Use `hwr-pump.yaml` with real device configuration for actual hardware testing.

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
3. **Test After Changes**: Verify `hwr-pump-example.yaml` compiles and `hwr-pump.yaml` works on hardware.
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
* [x] HA services (`pump_set_enabled`, `pump_set_mode`, `pump_set_setpoint`, `pump_set_temperature_range`, `pump_set_cycle_times`) registered in C++ (`api_bridge`)
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
8. **Verify**: Compile `hwr-pump-example.yaml`, flash `hwr-pump.yaml`, and test on hardware.

### Adding a New Write Operation (issue #92 contract)

Any feature that WRITES to the pump must be a `WriteCommand` in
`services::WriteOperationService` — never a standalone write path:

1. **Define the command**: add it to `WriteCommand`, the `Operation` fields, and `write_command_to_string()`.
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

The `components/dhw_demand` component addresses a fundamental ambiguity in hot-water recirculation systems: **a flow sensor in the DHW circuit cannot distinguish closed-loop recirculation from an occupant actually opening a fixture**. The Droplet D1 sensor sits inline in the DHW circuit and reports flow regardless of source — when the ALPHA HWR pump is running a nonzero reading may be recirculation only, demand only, or both simultaneously. The ALPHA HWR's internal flow sensor (0–0.53 GPM) is blind to demand when the pump is idle.

The theoretical foundation is fully documented in the companion research project:

* **`docs/hot-water-research.md`** — exhaustive treatment of hydrodynamic and thermodynamic signatures that separate recirculation from genuine DHW demand. Key insight: the instant an occupant opens a valve the system topology changes from **closed-loop** to **open-loop**, producing measurable hydraulic transients (pressure drop, flow-rate collapse at the pump, current/power spike) that cannot be explained by normal recirculation.
* **`docs/esp32-detector.md`** — describes how the detection algorithm is ported to an ESP32/ESPHome environment, including sensor requirements, memory footprint, threshold defaults, and the MQTT output format.

**Design philosophy:** We take a **heuristic, threshold-based approach** — no ML, no model training, no InfluxDB dependency. Each physical signal votes independently; confidence is computed from vote weight and count. The thresholds are derived from observed hardware behaviour and can be tuned without reflashing via ESPHome `substitutions`.

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
| `motor_speed` | RPM | Primary pump-state indicator |
| `motor_current` | A | Fallback pump-state; current-spike derivative |
| `inlet_pressure` | PSI | Pressure transient derivative; absolute low-pressure floor |
| `pump_flow` | GPM | Pump-side flow collapse (reads 0 when pump is off) |
| `pump_power` | W | Power-spike derivative |

#### Supplementary sensors (fetched from Home Assistant via `platform: homeassistant`)

| Config key | Signal | Notes |
|---|---|---|
| `flow` | GPM | Droplet D1 inline DHW circuit meter; detects **both** recirculation and demand flow — only unambiguous when pump is off; **30-sample circular buffer** (5 min at 10 s grid) |
| `tank_lower_temp` | °F | Tank thermal collapse derivative |
| `dhw_charge` | % | DHW charge-drop derivative |
| `dhw_in_use` | boolean (as float) | NWP500 native flag; corroborates demand when pump is off |

### 11.4 Detection Algorithm

The tick runs in `update()` every 10 seconds. Steps:

1. **Read sensors & compute derivatives** — `Δx/Δt` using actual elapsed ms so jitter in the update interval doesn't bias rates.
2. **Push Droplet flow into 30-sample circular buffer** — used by the falling-edge latch.
3. **Determine pump state** — `motor_speed > 0` preferred; `motor_current ≥ pump_off_current_threshold` fallback.
4. **Run the appropriate detection branch** (pump-off or pump-on).
5. **DHW-in-use confidence boost** — +0.05 if `nwp500_dhw_in_use` corroborates demand.
6. **Publish results** and **update session tracking**.

#### Pump-OFF branch

When the pump is off the Droplet D1 reads only genuine demand flow, making it the unambiguous ground-truth signal. Three signals vote independently:

| Signal | Condition | Weight |
|---|---|---|
| Droplet flow | `flow > flow_threshold` (0.3 GPM) | 1.0 |
| Thermal collapse | `Δtemp/Δt < −thermal_collapse_rate` (0.05 °F/s) | 0.9 |
| Charge drop | `Δcharge/Δt < −dhw_charge_drop_rate` (0.005 %/s) AND tank not warming | 0.7 |

Confidence = highest-weight signal + 0.05 per additional corroborating signal, capped at 1.0.

**No-flow guard:** if current Droplet flow is below threshold *and* the 30-second falling-edge latch has expired, demand is suppressed regardless of other signals. This is the primary false-positive filter.

**Falling-edge latch:** if Droplet flow was above threshold within the last `flow_latch_seconds` (30 s) but has since dropped (burst-cadence gap), demand is held alive to prevent a single missed Droplet reading from causing a false termination.

#### Pump-ON branch

When the pump is running the Droplet D1 sees recirculation flow in addition to any demand and cannot be used as a direct indicator. Two sub-paths are checked in order:

1. **Continuation detection** — if Droplet flow was above threshold on the last pump-off tick and is still above threshold now, confidence = 0.85. This handles draws that were already in progress when the pump turned on.

2. **Deterministic hydraulic voting** — five signals each contribute one vote. These signals reflect the **open-loop topology change** caused by a fixture being opened and are not produced by normal recirculation:

   | Signal | Condition |
   |---|---|
   | Pressure transient | `|Δinlet/Δt| > 0.07 PSI/s` |
   | Inlet pressure floor | `inlet_psi < 5.0 PSI` |
   | Pump flow collapse | `pump_flow < 0.2 GPM` |
   | Current spike | `|Δcurrent/Δt| > 0.001 A/s` |
   | Power spike | `Δpower/Δt > 5.0 W/s` |

   Confidence scales with vote count: 1→0.50, 2→0.65, 3→0.80, 4→0.90, 5→0.95. One vote is sufficient to declare demand.

   > **Why thermal/duration paths are disabled during pump-on:** During long recirculation runs the pump returns progressively cooled water to the tank cold inlet, causing the lower tank temperature to drop at rates up to −0.083 °F/s — indistinguishable from a shower draw. Only hydraulic signals (which reflect the open-loop topology change) are reliable discriminators.

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
| `inlet_pressure_transient_threshold` | 0.07 | PSI/s | Valve-shock detection |
| `inlet_pressure_demand_floor` | 5.0 | PSI | Absolute low-pressure threshold |
| `pump_flow_collapse_threshold` | 0.2 | GPM | Pump-side flow collapse |
| `motor_current_spike_threshold` | 0.001 | A/s | Current rate of change |
| `pump_power_spike_threshold` | 5.0 | W/s | Power rate of change |
| `flow_latch_seconds` | 30 | s | Falling-edge hold-off duration |
| `session_gap_tolerance_seconds` | 60 | s | Max gap before ending a session |

### 11.8 Development Rules for `dhw_demand`

1. **No ML, no external dependencies** — the component must compile and run entirely on-device with no Python runtime, no InfluxDB, no trained model. All logic is explicit threshold-based heuristics.
2. **All inputs optional** — never `assert` or crash on a missing sensor. Missing signals return `NAN`; detection paths that require a `NAN` input are silently skipped.
3. **Threshold changes are config changes, not code changes** — if a threshold needs tuning, adjust via YAML `substitutions`, not by editing `.cpp`.
4. **Derivatives use actual elapsed time** — always divide by the real `dt_s` from `millis()` delta, not an assumed 10-second interval.
5. **Consult reference docs before changing thresholds** — `esp32-detector.md` explains the physical rationale for every default. Changes should be grounded in observed hardware behaviour.
6. **Test compilation** — `dhw_demand` has no BLE dependency; verify it compiles by including it in `hwr-pump-example.yaml` or a minimal test YAML.
7. **Logging discipline** — follow the same `ESP_LOGx` conventions as `alpha_hwr`: `LOGV` for per-tick data, `LOGD` for state transitions, `LOGI` for session start/end.
