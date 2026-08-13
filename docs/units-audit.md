# Units Audit

This is the ground-truth reference for how every entity interprets its units.
It exists because a single wrong scale factor is hard to spot and easy to
misattribute: the Constant Flow setpoint m³/h-vs-m³/s mismatch (#88/#90)
root-caused a whole class of apparently unrelated bugs (#44, #81, #96, #83)
before it was understood. Check any new entity against this table — and against
the authoritative resources it cites — before adding it.

## How values reach physical units

The design bets that **live telemetry** arrives from the pump as raw IEEE-754
big-endian floats **already in physical units** — no GENI unit-index scaling,
no offsets. This was verified field-by-field against the reference decoders and
the captured traffic (see "Re-verify" below). Only **setpoints, trends, and
statistics** apply explicit conversion factors, all hardcoded and listed below.

> GENI can also encode values as scaled integers keyed by a unit-index byte
> (see `unit_index_mapping.csv`). This pump's telemetry uses the extended-float
> responses instead, so the unit-index tables are the reference for *setpoint*
> registers and cross-checking, not for the live-telemetry decode path.

## Telemetry (raw physical floats — no scaling)

| Entity | Unit | Decode | Confirmed against |
|---|---|---|---|
| Flow Rate | m³/h | `telemetry_decoder.cpp` `decode_flow_pressure_response` off 37 | `telemetry_decoder.py:608-609` (float m³/h, no ×3600) |
| Head | **m** | same, off 41 | `telemetry_decoder.py:613` (float m) |
| Inlet Pressure | bar | same, off 45 | `telemetry_decoder.py:230` |
| Power | W | `decode_motor_state_response` off 25 | reference float layout |
| Motor Speed | RPM | same, off 33 | reference (raw float, **not** unit-index ×100) |
| AC / DC Voltage | V | same, off 13 / 17 | reference |
| Motor Current | A | same, off 21 | reference |
| Water / PCB / Control-box Temp | °C | `decode_temperature_response` off 13 / 17 / 21 | `telemetry_decoder.py:283-295` (float °C, no deci-K/offset) |

All of the above are pinned by `tests/test_telemetry_units.cpp`.

## Derived / converted (explicit factors)

| Entity | Unit | Factor | Code | Confirmed against |
|---|---|---|---|---|
| Head Rate | m/s | d(head_m)/dt | `sensor_publisher.cpp` head callback | derived from head (m) |
| Constant / Proportional Pressure setpoint | m | Pa ÷ 9806.65 (read), m × 9806.65 (write) | `control_service.cpp:140-141`, `write_operation_service.cpp:453` | `control.py:442,566,665` |
| Constant Flow / Cycle Flow setpoint | m³/h | m³/s × 3600 (read), m³/h ÷ 3600 (write) | `control_service.cpp:142`, `control_service.h`, `write_operation_service.cpp:455` | `unit_factor_mapping.csv` (m³/h↔m³/s = 3600) + #88 bench fix |
| Constant Speed setpoint | RPM | none (native) | `write_operation_service.cpp:457` | native |
| History trend Flow / Head samples | m³/h / m | uint8 × 0.1 | `history_service.cpp` `TREND_CONFIGS` | `history.py:17-20` (identical scales) |
| History trend Temperature samples | °C | uint8 × 1.0 | same | `history.py:19` |
| Operating Hours | h | seconds ÷ 3600 | `device_info_service.cpp:236` | `device_info.py:349` |
| DHW Detection Confidence | % | internal 0–1 × 100 | `dhw_demand.cpp` `publish_result_` | ratio → percent |

Setpoint round-trips (÷3600, ÷9806.65 and their inverses) are exercised
end-to-end in `tests/test_write_operations.cpp` against the pump simulator.

## Notes on deliberate choices

- **Head is meters, not kPa.** Meters of head is the pump's native domain unit
  (Grundfos GO app, datasheet pump curves, GENI Head/Distance) and matches the
  pressure setpoints. Because `m` is not a valid Home Assistant `pressure`
  device_class unit, the Head sensor is classed `distance` instead (issue #157):
  meters of head is dimensionally a length, and HA's `distance` class accepts
  both `m` and `ft`. That is display metadata only — the published state, the
  decode path and the recorded unit are all still meters — but it gives HA the
  per-entity unit picker, so the value can be shown in feet, the unit the
  Grundfos manual leads with. Head Rate follows as m/s, with no device_class
  (`speed` would be defensible dimensionally; it is diagnostic and carries an
  explicit icon, so there is little to gain). The DHW detector used to threshold
  it via `pump_head_rate_threshold` (`0.31` m/s, ≈ the former `3.0` kPa/s ÷
  9.80665); that key and the head-rate vote it fed were retired in issue #149,
  so the unit choice now only affects the published Head Rate sensor.
- **Two flow conventions are both correct.** Telemetry flow is extended-float
  m³/h; the Object-86 flow *setpoint* register is SI m³/s (×3600). They are
  different GENI encodings, not a mismatch — this is the axis of the #88 bug.

## Re-verify

Authoritative resources (sibling repo `alpha-hwr/resources/`, subset vendored
under `reference/`):

1. `reference/alpha-hwr/src/alpha_hwr/protocol/telemetry_decoder.py`,
   `services/control.py`, `services/history.py` — the reference decoders.
2. `resources/unit_conversion/unit_index_mapping.csv`,
   `unit_factor_mapping.csv` — GENI unit-index → multiplier and unit→unit
   factors (the `3600` flow factor, the m↔bar/psi/cm factors).
3. `resources/geni_tools/unit_map.json`, `parameters_52_7.json`,
   `resources/geni_profile_52_7.xml` — the pump's object/sub/parameter model.
4. `resources/traffic_db/traffic_decoded.db` and `resources/traffic_capture/*.log`
   — real BLE frames (e.g. `constant_flow.log`) for byte-level spot checks.

To audit a new entity: find its GENI object/sub in the profile
(`geni_profile_52_7.xml` / `parameters_52_7.json`), read its unit-index or the
narrative mapping in `GRUNDFOS_APP_BLE_DATA_MAPPING.md`, look up the
multiplier/unit in `unit_index_mapping.csv`, and confirm against a real frame in
the capture DB. Then add a row here and an assertion in
`tests/test_telemetry_units.cpp`.
