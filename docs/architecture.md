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

## Adding New Features

1. Identify the layer: `protocol::` for packet encoding, `core::` for transport/state, `services::` for business logic.
2. Cite the [GENI protocol doc](https://eman.github.io/alpha-hwr/reimplementation/) for any packet formats.
3. Unit-test packet builders against known byte sequences before flashing.
4. If the feature *writes* to the pump, add it as a `WriteCommand` in the
   operation layer (wire steps + confirm comparator + host test) rather than
   as a standalone write path — see AGENTS.md §9.
