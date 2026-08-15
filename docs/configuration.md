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
| `data_timeout` | time | `60s` | Tear the BLE link down after this long with no data from the pump, so the normal reconnect runs. Set to `0s` to disable. |

### `data_timeout`

The GENI handshake does not verify that the pump is answering: authentication
sends its packets on timers and never inspects a reply, so the session reaches
its ready state whether or not anything came back. A link whose writes succeed
but whose notifications never arrive therefore reports itself healthy
indefinitely — every sensor frozen, nothing to trigger a reconnect, because the
BLE connection itself is fine.

`data_timeout` closes that hole with a liveness check: if nothing is received
for this long while connected, the link is dropped and the usual reconnect
takes over. During the reconnect the **Pump Link Fault** sensor reads
`No data from pump (60s)` — held there rather than overwritten by the local
disconnect that caused it — and returns to `None` once data flows again.

The window is timed from connection-open and refreshed on every received
notification. A working link is polled every 10 seconds, so in steady state the
default tolerates five missed poll cycles and acts on the sixth.

This recovers a link that *can* recover. A pump that is permanently deaf will
instead cycle — roughly 60 seconds looking connected, then about 6 seconds
reconnecting — because reaching the ready state still does not require data. If
you see the link status and fault sensors flapping on that cadence, the pump
itself is not answering and reconnecting will not fix it.

Raise `data_timeout` if your setup has long legitimate quiet periods; set it to
`0s` to disable the check entirely.

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
    name: "Head"
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
degrades gracefully as inputs drop out (an unavailable sensor reads NaN and the
rule that needed it declines rather than guessing).

> **Breaking change in this release (issue #149).** The pump-on hydraulic vote
> tier was replaced by a direct measurement, and nine keys went with it:
> `inlet_pressure`, `pump_power`, `pump_head_rate`,
> `inlet_pressure_transient_threshold`, `inlet_pressure_demand_floor`,
> `pump_flow_collapse_threshold`, `motor_current_spike_threshold`,
> `pump_power_spike_threshold` and `pump_head_rate_threshold`.
>
> A config that still sets one **fails at `esphome config` time** with a message
> naming the replacement — deliberately, rather than being accepted and ignored,
> because config that validates but does nothing is worse than config that
> breaks. **To migrate: delete them.** Nothing replaces them one-for-one; the
> new keys are listed under Thresholds below and all four have working defaults.
>
> Make sure `pump_flow` and `motor_speed` *are* wired. They are now what pump-on
> detection runs on. Without them the detector still works while the pump is off
> and reports `pump_on_uncertain` whenever it is running.

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
The two move independently — a small draw can be measured with high confidence.
Intensity is `min(1.0, GPM / 2.5)` in both pump regimes: household flow while the
pump is off, and *computed* demand (`flow − pump_flow`) while it is on. It is not
a flow rate and should not be read as one. While `demand_release_seconds` is
latching, the last live value is republished rather than 0.0.

Confidence on the pump-on branch rises with how far the measured draw clears the
threshold rather than with a count of agreeing signals:
`min(0.90, 0.60 + 0.30 × margin)`. The 0.90 cap reflects that the measurement is
a difference of two instruments, not a direct reading of the tap.

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
| `pump_flow` | sensor id | — | Pump's own recirculation-loop flow (GPM). Subtracted from `flow` to measure household demand while the pump runs |
| `dhw_in_use` | sensor id | — | The heater's own DHW in-use flag. Applies a +0.05 confidence boost while the pump is off, and — once continuously high for `dhw_in_use_min_seconds` — can declare a pump-on draw on its own as the last-resort recall tier |

### Thresholds

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `pump_off_current_threshold` | float | `0.03` | A — below this the pump counts as off |
| `flow_threshold` | float | `0.3` | GPM. Also the no-flow guard, the detector's primary false-positive filter. Draws below this are not detected — a steady 0.11–0.16 GPM draw measures as `deterministic_idle`. Measured over 10.4 days before leaving it here (#180): sub-threshold pump-off readings are 3.2× enriched within 30 s of a pump-off edge, so lowering the floor admits the pump's own collapsing loop flow, and 89 % of the rest fall within 60 s of a reading already above 0.3 — the shoulders of draws already detected. Isolated small draws run about one per ten days, and a lower floor buys 0 s of median onset lead. See `AGENTS.md` §11.4 before changing it. |
| `thermal_collapse_rate` | float | `0.05` | °F/s — tank cooling faster than this signals a draw |
| `dhw_charge_drop_rate` | float | `0.005` | %/s — charge falling faster than this signals a draw |
| `pump_on_demand_flow_threshold` | float | `0.3` | GPM of **computed** demand (`flow − pump_flow`) above which a pump-on draw is declared. Shares `flow_threshold`'s value deliberately, so both pump regimes agree on what counts as flow |
| `pump_on_demand_min_speed_rpm` | float | `1950` | RPM below which the subtraction is not trusted. The pump *estimates* its loop flow rather than metering it, and below ~2000 RPM the estimate reads low, so the difference goes spuriously positive with no draw (+0.45 GPM measured at 1650). Admits the whole production range — the pump's own 29-day minimum was 1971 RPM |
| `pump_on_demand_max_stale_seconds` | int | `30` | s — how old the `pump_flow` reading may be and still be differenced. The pump reports every 10 s |
| `flow_max_stale_seconds` | int | `60` | s — the same bound for `flow`. Deliberately looser: the meter reports on change, at a median 28 s while flowing, so matching the pump's 30 s would reject half of normal cadence |
| `dhw_in_use_min_seconds` | int | `70` | s — how long `dhw_in_use` must stay **continuously** high before it may declare a pump-on draw on its own. The flag fires ~77 times a day with a median duration of 15 s, so it is unusable bare; 70 s clears 89.7 % of its events. `0` accepts the bare flag (bench/debug only). A NaN sample breaks the run exactly as a low one does, so a BLE dropout resets the timer rather than holding the last value |
| `flow_latch_seconds` | int | `30` | s — how long flow keeps counting after it stops |
| `pump_on_demand_settle_seconds` | int | `10` | s — how long after a pump start the subtraction declines, because the pump's own loop-flow estimate is still spinning up and reads low against a loop the meter already sees moving. Measured across 296 starts: 0–10 s is p90 0.820 GPM with 26.6 % above the 0.3 threshold, against a flat 6–9 % from 10 s out to 180 s. Not covered by `pump_on_demand_min_speed_rpm`, which is for a low *steady* speed — this fires at 3172 RPM during the overshoot. `0` disables |
| `pump_on_continuation_max_seconds` | int | `300` | s — how long the continuation tier may keep asserting a draw that no measurement has *supported* for that long. A subtraction clearing `pump_on_demand_flow_threshold` re-stamps the support time, so this is not a ceiling on the pump run: a draw being measured tick after tick never expires. It bounds the blind case — chiefly a pump turning below `pump_on_demand_min_speed_rpm`, where no measurement of household draw exists to support or contradict the claim. `0` disables the tier rather than unbounding it |
| `latch_pump_off_suppression_seconds` | int | `30` | s — disarms the latch above for this long after a pump-off edge. Loop flow collapses through the flow threshold within seconds of the motor parking, so without this the latch is armed by the pump's own flow and holds a thermal/charge verdict alive on it. Defaults to the latch's own reach, so no shutdown reading can arm it. `0` disables |
| `session_gap_tolerance_seconds` | int | `60` | s — a lull shorter than this does not end a session |
| `demand_release_seconds` | int | `30` | s — how long demand stays latched after the last positive tick. Set to `0` to publish the raw per-tick result |
| `update_interval` | time | `10s` | Detection tick interval |

> The four `pump_on_demand_*` / `flow_max_stale_seconds` keys only matter when
> `pump_flow` and `motor_speed` are wired.

> Migration: `flow_max_stale_seconds` was previously spelled
> `droplet_max_stale_seconds`, after the meter one installation happened to
> use. Behavior and default are unchanged; a config still setting the old name
> fails validation naming the replacement.

> The two staleness bounds exist because a difference of two quantities is only
> meaningful if both are current, and ESPHome carries no provenance on a sensor
> value — `has_state()` says a reading exists, not how old it is. Measured over
> 30 days, gaps in the pump's flow channel beyond 20 s are 1 % of gaps but **14 %
> of pump running time**, so a last-known-value read is stale about a seventh of
> the time the pump runs.

> Unit trap: `alpha_hwr` publishes flow in m³/h while the detector expects GPM.
> Convert with a `platform: copy` sensor and a `multiply` filter
> (1 m³/h = 4.40287 GPM) — see the header comment in
> `packages/dhw_demand_detector.yaml`.

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
| `deterministic_continuation` | Draw was already active when the pump started, and neither the subtraction nor `pump_on_continuation_max_seconds` has ended it |
| `deterministic_pump_on_subtraction` | Pump on, and `flow − pump_flow` measured a draw |
| `flow_onset_pending` | First tick of flow, awaiting confirmation |
| `demand_release_hold` | No live signal; demand latched by `demand_release_seconds` |
| `deterministic_dhw_in_use` | Pump on, nothing above it fired, and the heater's flag has been continuously high past `dhw_in_use_min_seconds` |
| `pump_on_uncertain` | Pump running, and nothing could separate a draw from recirculation — either the subtraction measured no draw, or one of its guards declined (a channel missing, a reading stale, or the pump below `pump_on_demand_min_speed_rpm`) |
| `deterministic_idle` | No demand, high confidence |
| `no_flow` | Flow below threshold and the latch has expired |
