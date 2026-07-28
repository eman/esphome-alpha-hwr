# Configuration

## `api:` requirements

The component's programmatic write services and the
`esphome.alpha_hwr_write_settled` event (see
[programmatic-interface.md](programmatic-interface.md)) require two flags on
the ESPHome `api:` component:

```yaml
api:
  custom_services: true
  homeassistant_services: true
```

All shipped packages and examples set these. Without them the component still
compiles and the entities work, but no services are registered and no settle
events fire.

> Naming note: the existing `reconnect_settle_time` option below is unrelated
> to the `write_settled` event — it is a BLE reconnect hold-off.

## alpha_hwr Component

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `ble_client_id` | string | **required** | BLE client ID for pump connection |
| `enable_pairing` | boolean | `false` | Enable BLE pairing for control and enhanced telemetry |
| `reconnect_settle_time` | time | `2s` | Delay after disconnect before reconnecting |
| `control_state_poll_interval` | time | `30s` | Interval for periodic control state polling. Set to `0s` to disable. |

## Examples

### Basic read-only monitoring
```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  control_state_poll_interval: 0s
  flow:
    name: "Flow Rate"
  rpm:
    name: "Motor Speed"
```

### Full control with state polling
```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 30s
  flow:
    name: "Flow Rate"
  head:
    name: "Head Pressure"
  rpm:
    name: "Motor Speed"
  voltage:
    name: "AC Voltage"
  current:
    name: "Motor Current"
```

## Build Identification

Two diagnostic entities answer "what is actually running on this node?", which
matters when correlating a behavior change with an install (issue #124):

| Entity | Source | Example |
| --- | --- | --- |
| `Component Version` | `${component_version}` substitution in the package, published at boot | `0.13.0` |
| `Component Build` | `component_build:` on `alpha_hwr:` | `v0.13.0-30-g066640c-dirty (built 2026-07-27 20:08:58 -0700)` |

`Component Version` is the **release** version — it changes only when a release
is cut, so every build between two releases reports the same value. Use it to
answer "which release is this?".

`Component Build` identifies the **build**. The revision is `git describe` of
the component's source tree, resolved at compile time: ESPHome clones
`github://` sources with git and a `type: local` source is your working tree, so
both resolve (`-dirty` marks uncommitted changes; `unknown` means git wasn't
available, e.g. a tarball install). The firmware build timestamp is appended, so
two flashes of the same revision are still distinguishable. Include this value
in bug reports.

## Node Health Diagnostics

Both packages expose the ESP32's runtime heap through ESPHome's `debug`
component, polled every 60 s (issue #127):

| Entity | What it answers |
| --- | --- |
| `Free Heap` | Headroom right now |
| `Min Free Heap` | The worst moment since boot — the one that survives a spike you weren't watching |
| `Largest Free Block` | Fragmentation's practical effect: a large allocation fails on this, not on `Free Heap` |
| `Heap Fragmentation` | Trend, as a percentage |
| `Reset Reason` | Why the node last rebooted — separates a panic from an OTA or a power cut |

The build's static-RAM figure does not capture any of this: the interesting
allocations happen at runtime with the BLE stack up and the API queueing
outgoing frames.

### Log level and API subscribers

The packages set `logger: level: INFO`. Every log line, like every state change,
is an API frame fanned out to **each** connected subscriber, so a node with Home
Assistant plus an `esphome logs` stream plus a polling script pays each DEBUG
line three times — and the node has run out of heap inside ESPHome's outgoing
API buffer under exactly that load. Raise it deliberately when troubleshooting
(your own config wins over the package):

```yaml
logger:
  level: DEBUG
```

and prefer keeping at most one extra subscriber attached beyond Home Assistant
while you do. Watch `Free Heap` / `Min Free Heap` during long debug sessions.

## Control State Polling

Periodically reads pump control state to detect changes from internal schedules, manual button presses, or external apps. Keeps component state synchronized with pump reality.

**Default:** 30 seconds polling  
**Disable:** Set to `0s` if you guarantee exclusive component control  
**Customize:** Use any time interval (e.g., `60s`, `15s`)

Polling is non-blocking and doesn't impact other component operations. Failures are logged but don't break anything.

It is also what keeps the `pump_run_state` / `schedule_stalled` entities honest
and drives the stalled-schedule repair (see
[schedule-management.md](schedule-management.md#the-stalled-schedule-and-how-it-repairs-itself)).
With polling set to `0s`, run state is only re-read after the component's own
writes, so a stall introduced by the Grundfos GO app is not noticed until the
next reconnect.

## dhw_demand Component

Infers hot-water draws from pump and tank telemetry. Independent of `alpha_hwr`;
see [Architecture](architecture.md#dhw-demand-detection) for how the detection
works. Every key is optional — the detector uses whatever sensors it is given and
degrades gracefully as inputs drop out (an unavailable sensor reads NaN and simply
casts no vote).

### Output sensors

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `demand` | binary_sensor | — | Whether a draw is in progress |
| `confidence` | sensor | — | Detection confidence, 0–100 % |
| `demand_level` | sensor | — | Estimated draw intensity, 0.0–1.0 |
| `session_duration` | sensor | — | Live duration of the current draw, seconds |
| `detection_method` | text_sensor | — | Which rule fired (e.g. `deterministic_flow`) |

`confidence` and `demand_level` answer different questions: confidence is how sure
the detector is that a draw is happening, `demand_level` is how large it looks.
The two move independently — a single hydraulic vote is a confident-enough
detection at a low intensity. Intensity is derived from flow where flow is
available (`min(1.0, GPM / 2.5)`), and from the vote count on the pump-on
hydraulic branch (`0.3 + 0.15 × (votes − 1)`), matching the Python detector. It
is not a flow rate and should not be read as one. While
`demand_release_seconds` is latching, the last live value is republished rather
than 0.0.

The two also count votes differently, deliberately (issue #125). This firmware
has a sixth pump-on signal the Python detector does not — a head-pressure rate
spike, cheap here because the pump publishes head natively over BLE, and closed
on the Python side because that detector deprecated its head channel. It feeds
`confidence`, which is each detector's judgement of its own evidence, but not
`demand_level`, which is part of the cross-detector contract and must mean the
same thing whichever detector is publishing it. So on the pump-on branch
`demand_level` counts only the five shared signals and tops out at **0.90**,
exactly as Python does; a value of 1.0 arrives only from the flow branch.

All five outputs publish **on change only** (issue #129), not once per
`update_interval`. They are step-valued — `detection_method` names the rule that
fired, `confidence` and `demand_level` are flat between transitions, and
`session_duration` is `0` while nothing is drawing — so a per-tick republish sent
an API state frame to every subscriber carrying no new information. A running
draw is unaffected (`session_duration` moves every tick while a session is open),
and Home Assistant still gets every entity's current state on connect, so this is
invisible except in state traffic. If you need one of these as a liveness
heartbeat, set `force_update: true` on that sensor — it restores the per-tick
publish and also tells Home Assistant's recorder to store every repeat:

```yaml
dhw_demand:
  confidence:
    name: "DHW Detection Confidence"
    force_update: true
```

`detection_method` is a `text_sensor` and has no `force_update`; use `demand` or
the node's own connection state for availability instead.

### Input sensors

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `flow` | sensor id | — | Household hot-water flow (GPM). The ground-truth signal while the pump is off |
| `tank_lower_temp` | sensor id | — | Tank lower temperature (°F); drives the thermal-collapse signal |
| `dhw_charge` | sensor id | — | Tank charge (%); drives the charge-drop signal |
| `motor_speed` | sensor id | — | Pump RPM; selects the pump-on/pump-off branch |
| `motor_current` | sensor id | — | Pump current (A); branch fallback when RPM is absent |
| `inlet_pressure` | sensor id | — | Pump inlet pressure (PSI) |
| `pump_flow` | sensor id | — | Pump-side flow (GPM) |
| `pump_power` | sensor id | — | Pump power (W) |
| `pump_head_rate` | sensor id | — | Rate of change of head pressure (m/s) |
| `dhw_in_use` | sensor id | — | External in-use flag; applies a +0.05 confidence boost while the pump is off |

### Thresholds

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `pump_off_current_threshold` | float | `0.03` | A — below this the pump counts as off |
| `flow_threshold` | float | `0.3` | GPM — flow above this is a draw |
| `thermal_collapse_rate` | float | `0.05` | °F/s — tank cooling faster than this signals a draw |
| `dhw_charge_drop_rate` | float | `0.005` | %/s — charge falling faster than this signals a draw |
| `inlet_pressure_transient_threshold` | float | `0.07` | PSI/s — valve-open pressure shock |
| `inlet_pressure_demand_floor` | float | `5.0` | PSI — inlet below this suggests an open circuit |
| `pump_flow_collapse_threshold` | float | `0.2` | GPM — pump-side flow diverted to the house |
| `motor_current_spike_threshold` | float | `0.001` | A/s — load change at valve opening |
| `pump_power_spike_threshold` | float | `5.0` | W/s — corroborates the current spike |
| `pump_head_rate_threshold` | float | `0.31` | m/s — corroborating vote only; never fires alone |
| `flow_latch_seconds` | int | `30` | s — how long flow keeps counting after it stops |
| `session_gap_tolerance_seconds` | int | `60` | s — a lull shorter than this does not end a session |
| `demand_release_seconds` | int | `30` | s — how long demand stays latched after the last positive tick. Set to `0` to publish the raw per-tick result |
| `update_interval` | time | `10s` | Detection tick interval |

> The five pump-on threshold keys only matter when the corresponding pump sensors
> are wired. Note that `pump_head_rate` (the sensor) and `pump_head_rate_threshold`
> (its trigger level) are different keys.

> Unit trap: `alpha_hwr` publishes flow in m³/h and pressure in bar, while the
> detector expects GPM and PSI. Convert with a `platform: copy` sensor and a
> `multiply` filter (1 m³/h = 4.40287 GPM, 1 bar = 14.5038 PSI) — see the header
> comment in `packages/dhw_demand_detector.yaml`.

### Example

```yaml
dhw_demand:
  update_interval: 10s
  demand:
    name: "DHW Demand"
  confidence:
    name: "DHW Detection Confidence"
  demand_level:
    name: "DHW Demand Level"
  session_duration:
    name: "DHW Session Duration"
  detection_method:
    name: "DHW Detection Method"
  flow: dhw_flow
  tank_lower_temp: dhw_tank_lower_temp
  dhw_charge: dhw_charge_pct
  flow_threshold: 0.3
  demand_release_seconds: 30
```

To diagnose detection behavior, watch the component's own `confidence` and
`detection_method` sensors — `detection_method` names the exact rule that fired on
each tick, and the component logs a per-tick `VERBOSE` line with every input.

| `detection_method` | Meaning |
| --- | --- |
| `deterministic_flow` | Household flow above threshold (pump off) |
| `deterministic_thermal` | Tank temperature collapsing |
| `deterministic_charge` | Tank charge dropping |
| `deterministic_continuation` | Draw was already active when the pump started |
| `deterministic_pump_on` | Pump-on hydraulic votes carried the tick |
| `flow_onset_pending` | First tick of flow, awaiting confirmation |
| `demand_release_hold` | No live signal; demand latched by `demand_release_seconds` |
| `pump_on_uncertain` | Pump running, cannot separate a draw from recirculation |
| `deterministic_idle` | No demand, high confidence |
| `no_flow` | Flow below threshold and the latch has expired |
