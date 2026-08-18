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
- **`api_bridge`** — Home Assistant surface of the programmatic write interface: registers the pump and schedule write services — each named after the
  `WriteCommand` it settles as, so the service you call and the event's
  `command` field are one string (issue #159) — and fires the terminal `esphome.alpha_hwr_write_settled` event. Compiled only when the `api:` component enables `custom_services` + `homeassistant_services`.
- **`core::`** — Manages BLE I/O and connection state. The transport uses a command queue and 3-state FSM (`IDLE` → `SENDING_CHUNKS` → `AWAITING_RESPONSE`) to stay non-blocking inside ESPHome's event loop.
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
- **Time sync**: Automatic daily RTC synchronization via SNTP; initial sync fires 2 seconds after the session becomes ready.
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
  started, and nothing since has ended it → 0.85), then the *subtraction* — `flow − pump_flow`
  is household demand directly, because the meter reads everything leaving the
  mains and the pump reports its own loop. Confidence rises with how far the
  measured draw clears the threshold, capped at 0.90. Otherwise
  `pump_on_uncertain`.
- **Ending the continuation is the hard part.** Its original exit was "and flow
  is still above threshold now", which cannot go false while the pump runs — the
  meter is reading the recirculation loop, which clears the threshold by at
  more than 2x at every speed recorded here. A draw that stopped mid-run therefore
  kept publishing demand until the pump did. It now ends on the subtraction
  measuring the draw as over (which also retires the stored evidence, so a later
  loss of the subtraction cannot resurrect a disproved claim), or on an expiry
  for the case where the subtraction is never available at all — a pump turning
  below its speed floor offers no measurement of household draw for the whole
  run. See `pump_on_continuation_verdict()` and audit finding 10.
- **The subtraction's three guards are all load-bearing**: both channels present;
  both readings current (30 s for the pump, 60 s for the meter — they do not
  report alike); and the pump turning above `pump_on_demand_min_speed_rpm`,
  because it *estimates* its loop flow rather than metering it and reads low near
  the bottom of its range. Each declines to NaN rather than guessing, and the
  branch falls through to `pump_on_uncertain`.
- **This replaced a five-signal hydraulic vote tier**, and the replacement is not
  to be undone. The two absolute votes were scalars on quantities that move with
  pump speed, so a quiet loop can sit 6 PSI below a drawing one; 73 % of the
  derivative votes' firings over 29 days landed within 25 s of a self-initiated
  pump speed change. Scored on the same cells: votes 0.530 precision with 23
  pump-on false positives, subtraction 0.808 with 0. See
  [issue #149](https://github.com/eman/esphome-alpha-hwr/issues/149).
- **No pump-on rule may key off raw meter flow.** Pump-on runs with no draw read
  a median 1.31 GPM against 1.74 with a draw — near-total overlap, so no
  threshold exists. The "~2.2 GPM recirculation baseline" quoted for years was
  the p90 of the no-draw case. This is settled, not merely untried; see
  [issue #138](https://github.com/eman/esphome-alpha-hwr/issues/138).
- **`dhw_in_use` recall tier**: below the subtraction sits one more path — the
  heater's own DHW in-use flag, once it has been *continuously* high for
  `dhw_in_use_min_seconds` (70). The flag is far too noisy bare (~77 events/day,
  median 15 s), and the guard is what makes it usable; its survivors corroborate
  against a channel sharing no sensor with it, the lower tank falling a median
  −0.390 °F/min when it fires against −0.043 when it stays silent. Where no
  measurement is available there is no honest intensity to publish, so it
  reports the shared no-claim constant 0.4 rather than deriving one from meter
  flow. (The "9 windows totalling ~20 minutes a week" footprint this used to
  quote, and the −0.390 °F/min figure, were both measured on the *ungated*
  tier — see the bullet below. Neither has been re-measured since.)
  See [issue #138](https://github.com/eman/esphome-alpha-hwr/issues/138).
- **It is a recall tier only, and fires only where nothing measured the loop.**
  It used to be described as strictly additive — below everything, only ever
  adding demand — and that was true of its *ordering* but not of its effect. A
  tier reached because the one above it declined can overrule that decline just
  as effectively as one that ran ahead of it. Where the subtraction has an
  answer and that answer is "no draw", the flag is now suppressed. Replaying 30
  days of stored data through the companion detector: of 1007 cells with the
  flag sustained, 937 had a measurement available and *all* of them measured no
  draw — the tier's marginal contribution over the subtraction, wherever the
  subtraction exists, is zero, and what it was adding there was recirculation.
  It keeps every corpus-confirmed true positive, all of which live in the
  no-measurement cells.

  Stated precisely, that is "not one *measured* draw", not "not one real draw":
  the only instrument that could confirm a real one is the subtraction being
  questioned. What the gate gives up is a true draw whose measured value lands
  at or under the cut — with the subtraction's −0.10 ± 0.06 GPM offset, a real
  draw up to roughly 0.4 GPM — and the evidence that none occurred is one house
  over one month. The trade is still the right one, but it is a trade. See
  [issue #173](https://github.com/eman/esphome-alpha-hwr/issues/173).
- **Release-hold on the output**: demand is recomputed from scratch each tick, so an
  input dithering around its threshold would chatter the binary sensor. Rising edges
  pass through immediately; falling edges are held for `demand_release_seconds`.
- **Pure logic lives in one header**: `dhw_demand_logic.h` holds the pump-on
  measurement and tier ordering, the pump-off signal predicates, the release hold
  and session accounting, with no ESPHome
  dependency and no `millis()` — anything time-dependent takes `now_ms` as a
  parameter. `tests/test_dhw_demand_logic.cpp` includes it directly and calls
  production code. Nothing in it may be hand-mirrored into a test; that drift is
  exactly what issue #120 was opened to eliminate.
- **The pump-on tier *ordering* is in that header too**, as
  `decide_pump_on(PumpOnInputs, PumpOnThresholds) -> PumpOnResult`. It used to
  live inline in `update()`, where the individual predicates were all under test
  but their composition was not — so "continuation outranks the subtraction" and
  "`pump_on_uncertain` is the last resort" held only by reading the `.cpp`. That
  is the same gap that let a stale threshold survive the units audit and fed the
  onset predicate the wrong argument for months. `update()` now reads sensors,
  stamps each channel's reading age, and calls the decision.
  See [issue #144](https://github.com/eman/esphome-alpha-hwr/issues/144).

Two questions used to be recorded here as open and deliberately un-actioned:
whether the `inlet_pressure_low` + `pump_flow_collapse` pair fires during pump
ramp-up, and whether the 5.0 PSI inlet floor was correctly placed. Both are
**closed by retirement** (issue #132, superseded by #149). The bench data they
waited on arrived and answered a prior question instead: no-draw inlet pressure
runs 3.4 PSI at 1650 RPM to 13.0 at 3600, while a real draw reads 5.7 at 2400 —
the populations overlap in the wrong direction, so no value of that floor works
anywhere. The votes were replaced rather than retuned.

## Adding New Features

1. Identify the layer: `protocol::` for packet encoding, `core::` for transport/state, `services::` for business logic.
2. Cite the [GENI protocol doc](https://eman.github.io/alpha-hwr/reimplementation/) for any packet formats.
3. Unit-test packet builders against known byte sequences before flashing.
4. If the feature *writes* to the pump, add it as a `WriteCommand` in the
   operation layer (wire steps + confirm comparator + host test) rather than
   as a standalone write path — see AGENTS.md §9.
