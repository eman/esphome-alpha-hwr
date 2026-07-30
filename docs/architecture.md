# Component Architecture

The component follows a layered, service-based architecture.

## File Structure

```
components/alpha_hwr/
├── alpha_hwr.h/cpp              # Main component (thin facade, orchestration)
├── api_bridge.h/cpp             # HA services + write_settled event (issue #92)
├── core::                       # Foundation layer
│   ├── transport.h/cpp          # BLE I/O, command queue, FSM transaction manager
│   ├── session.h/cpp            # Connection state machine
│   ├── auth.h/cpp               # Authentication handshake
│   └── ble_connection_manager   # BLE connection lifecycle
├── protocol::                   # Protocol layer (stateless)
│   ├── codec.h/cpp              # Endianness-safe encoding/decoding, CRC
│   ├── frame_builder.h/cpp      # Build GENI request packets
│   ├── frame_parser.h/cpp       # Parse GENI responses
│   └── telemetry_decoder.h/cpp  # Decode Class 10 DataObjects
└── services::                   # Business logic layer
    ├── write_operation_service  # Write lifecycle: serialize, confirm, settle events
    ├── telemetry_service        # Sensor readings and polling
    ├── control_service          # Pump-state cache + control wire primitives
    ├── schedule_service         # Weekly schedule management
    ├── device_info_service      # Device ID strings + operating statistics
    ├── time_service             # Pump RTC synchronization
    ├── event_log_service        # Start/stop event history
    ├── history_service          # Trend data + cycle timestamps
    └── sensor_publisher         # Map telemetry to ESPHome sensors
```

## Layers

- **`alpha_hwr`** — Thin facade. Delegates all work to services. No direct protocol manipulation.
- **`api_bridge`** — Home Assistant surface of the programmatic write interface: registers the `pump_*` and schedule services and fires the terminal `esphome.alpha_hwr_write_settled` event. Compiled only when the `api:` component enables `custom_services` + `homeassistant_services`.
- **`core::`** — Manages BLE I/O, connection state, and authentication. The transport uses a command queue and 3-state FSM (`IDLE` → `SENDING_CHUNKS` → `AWAITING_RESPONSE`) to stay non-blocking inside ESPHome's event loop.
- **`protocol::`** — Stateless frame builders and parsers. Pure functions with no side effects. Fully unit-testable on host without hardware.
- **`services::`** — One service per domain. Each owns all operations for its area (telemetry, control, schedules, etc.).

## Write Operations (single write path)

Every pump write — entity or service — goes through
`services::WriteOperationService` (issue #92). The operation layer serializes
write *sequences* (the transport only serializes individual commands): exactly
one operation is in flight at a time, each builds its wire frames from the
arguments passed rather than from a possibly-stale cache, and each ends in
exactly one terminal result (`accepted` / `clamped` / `rejected` / `timeout` /
`superseded`) decided by a confirm readback of the pump's actual stored value.
Guaranteed-terminal paths (validation rejects, per-operation watchdogs,
disconnect termination) mean a client waiting on a result can never hang.
`ControlService` keeps the pump-state cache, the issue-#91
commanded-but-unconfirmed guards, and the wire primitives the operation layer
composes; it no longer owns any multi-step write sequencing.

## Key Design Notes

- **Non-blocking transport**: 50ms pacing between commands; only one command in flight at a time.
- **Response matching**: Flexible Object/Sub-ID matching handles pump firmware quirks (SubID 0 wildcard responses).
- **Time sync**: Automatic daily RTC synchronization via SNTP; initial sync fires 2 seconds after authentication.
- **Namespace organization**: ESPHome requires a flat file structure, so layering is achieved via C++ namespaces (`esphome::alpha_hwr::core`, `::protocol`, `::services`) rather than subdirectories.
- **Units & scaling**: live telemetry arrives as raw IEEE-754 floats already in physical units; only setpoints, trends, and statistics apply conversion factors. Every entity's unit and factor is catalogued, cross-referenced to the GENI unit tables, and regression-tested — see [Units audit](units-audit.md). Confirm any new entity's factor there before adding it.

## DHW Demand Detection

The `dhw_demand` component (`components/dhw_demand/`) infers whether hot water is
actually being drawn, which the pump itself never reports. It is independent of
`alpha_hwr` — it consumes plain ESPHome sensors and can run standalone — but the
two are normally paired, with pump telemetry feeding the detector.

- **Two branches, chosen by pump state**: the pump running changes what the
  hydraulics mean, so `update()` picks a branch each tick from `detect_pump_on_()`
  (motor speed ≥ 10 RPM, falling back to motor current). Pump state is
  forward-filled when both motor sensors are unavailable.
- **Pump-off branch**: three weighted signals — household flow (1.0), tank thermal
  collapse (0.9) and DHW charge drop (0.7). Flow alone is ground truth here, so
  confidence is the top weight plus 0.05 per extra corroborating signal.
- **Flow-onset debounce**: a first tick of flow is ambiguous — it may be a single
  noisy sample or recirculation flow carried over from pump-on. It is confirmed by
  a corroborating signal, or by the flow having been present on the previous tick.
  See `pump_off_flow_onset_is_confirmed()`.
- **Pump-on branch**: continuation first (demand was already active when the pump
  started and flow persists → 0.85), otherwise six hydraulic votes mapped to
  confidence by vote count, capped at 0.95 because the votes are not independent.
- **Startup-transient suppression**: a recirculation pump start produces the same
  pressure, current, power and head spikes as a valve opening, so for 15 s after
  the pump starts the four derivative votes are ignored. The two *absolute* votes
  (inlet pressure below floor, pump-side flow collapse) stay live — they are the
  open-circuit evidence a pump start does not fake.
- **Head-rate vote is deliberately firmware-only**: signal 6 has no counterpart in
  the companion Python detector, which deprecated its head channel. It is safe to
  diverge because it is gated on at least one other signal already having voted, so
  it can only sharpen an existing detection, never create one. See
  [issue #120](https://github.com/eman/esphome-alpha-hwr/issues/120).
- **Release-hold on the output**: demand is recomputed from scratch each tick, so an
  input dithering around its threshold would chatter the binary sensor. Rising edges
  pass through immediately; falling edges are held for `demand_release_seconds`.
- **Pure logic lives in one header**: `dhw_demand_logic.h` holds the votes, the
  onset predicate, the release hold and session accounting, with no ESPHome
  dependency and no `millis()` — anything time-dependent takes `now_ms` as a
  parameter. `tests/test_dhw_demand_logic.cpp` includes it directly and calls
  production code. Nothing in it may be hand-mirrored into a test; that drift is
  exactly what issue #120 was opened to eliminate.
- **The pump-on tier *ordering* is in that header too**, as
  `decide_pump_on(PumpOnInputs, PumpOnVoteThresholds) -> PumpOnResult`. It used
  to live inline in `update()`, where the individual predicates were all under
  test but their composition was not — so "continuation outranks the votes" and
  "`pump_on_uncertain` is the last resort" held only by reading the `.cpp`. That
  is the same gap that let a stale threshold survive the units audit and fed the
  onset predicate the wrong argument for months. `update()` now reads sensors,
  resolves the startup-suppression window, and calls the decision.
  See [issue #144](https://github.com/eman/esphome-alpha-hwr/issues/144).

Parity with the Python detector is tracked in the companion repo's
[evaluation report](https://github.com/eman/dhw-sensor-apps/blob/main/docs/dhw-demand-detector-evaluation-2026-07.md),
[firmware change spec](https://github.com/eman/dhw-sensor-apps/blob/main/docs/esphome-alpha-hwr-parity.md)
and [ESP32 detector notes](https://github.com/eman/dhw-sensor-apps/blob/main/docs/esp32-detector.md).
Two questions there remain open and deliberately un-actioned here, because the
Python side has not moved either and acting alone would create fresh divergence:
whether the `inlet_pressure_low` + `pump_flow_collapse` pair (65 of 73 pump-on
detections in the evaluation window) fires during pump ramp-up, and whether the
5.0 PSI inlet floor is correctly placed. Both need bench data first.

## Adding New Features

1. Identify the layer: `protocol::` for packet encoding, `core::` for transport/state, `services::` for business logic.
2. Cite the [GENI protocol doc](https://eman.github.io/alpha-hwr/reimplementation/) for any packet formats.
3. Unit-test packet builders against known byte sequences before flashing.
4. If the feature *writes* to the pump, add it as a `WriteCommand` in the
   operation layer (wire steps + confirm comparator + host test) rather than
   as a standalone write path — see AGENTS.md §9.
