# Programmatic Write Interface (services + `write_settled` event)

This is the interface for anything that writes to the pump from a program — a
Home Assistant automation, pyscript, or a test harness. It replaces guessing
at internal timing with a simple contract:

1. Call a service, optionally passing an `op_id` you choose.
2. Wait for the single `esphome.alpha_hwr_write_settled` event carrying your
   `op_id`.
3. Read the result: what the pump actually stored, and whether your write was
   accepted, clamped, or rejected.

There are **no fixed delays to insert and no internal timing to know**. The
component performs the write, reads the settled value back from the pump, and
fires exactly one terminal event per call — including on rejection, timeout,
and disconnect — so a waiting client never hangs. (Design discussion:
issue #92.)

The entities remain the right tool for *reading* state and for a person
adjusting a value on a dashboard. Entity writes go through the same internal
write path (serialized and verified identically; their events carry
`op_id: ""`), but programmatic writes should use the services below.

## Requirements

The services and event need two flags on the `api:` component (all shipped
packages and examples set them):

```yaml
api:
  custom_services: true
  homeassistant_services: true
```

## Services

ESPHome prefixes each service with the node name: `esphome.<node_name>_<service>`
(`-` becomes `_`).

### Pump control

| Service | Arguments | What it writes |
| --- | --- | --- |
| `pump_set_enabled` | `enabled: bool`, `op_id: string` | Run state only, via the pump's unfused Class 3 START/STOP commands — carries no mode and no setpoint at all. A pump-rejected command settles `rejected`; the run state is always confirmed by readback (Class 3 produces no notification of its own). |
| `pump_set_mode` | `mode: string`, `op_id: string` | Control mode only, via the pump's unfused mode-change object — touches neither the run state nor any mode's stored setpoint. |
| `pump_set_setpoint` | `mode: string`, `value: float`, `op_id: string` | **Switches the pump into `mode` AND sets that mode's setpoint** — `mode` is not merely selecting which stored setpoint to edit; after this call the pump is running (or armed) in that mode. This is exactly the pair the pump fuses in one write. Use `pump_set_mode` to switch modes without touching a setpoint. The pump stores an independent setpoint per mode, so setpoint writes to different modes never supersede each other. |
| `pump_set_temperature_range` | `min_c: float`, `max_c: float`, `autoadapt: bool`, `op_id: string` | The temperature-range config object (its own fused write), after switching to temperature-range mode. |
| `pump_set_cycle_times` | `on_minutes: float`, `off_minutes: float`, `flow: float`, `op_id: string` | The cycle-time config object, after switching to cycle-time mode. Minutes are whole, 1–60 (float-typed for platform reasons; fractional values settle `invalid`). `flow` is the flow the pump targets during ON periods, in m³/h (0.1–10.0). **Each field accepts `0` = keep existing**: kept fields are resolved from a fresh read of the pump's stored config (a kept flow is echoed back byte-for-byte, no float round trip), so flow-only or single-period writes are safe. All three at `0` settles `invalid`. |
| `pump_set_state` | `state: string` (`off`\|`engaged`\|`scheduled`), `op_id: string` | **Coupled run-state + schedule selector** — the safe, one-call way to reach a legal state (see [Run state and the schedule](#run-state-and-the-schedule)). Writes only the flags that differ from the current state, in an order that never passes through the dead `STOP`+schedule combo. Unknown `state` settles `invalid`. |

`mode` strings: `constant_pressure`, `proportional_pressure`, `constant_speed`,
`constant_flow`, `auto_adapt_radiator`, `auto_adapt_underfloor`,
`auto_adapt_combined`, `cycle_time`, `temperature_range`.

Setpoint units and ranges: pressure modes in meters (0.5–10.0),
`constant_speed` in RPM (500–4500), `constant_flow` in m³/h (0.1–10.0).

### Run state and the schedule

`pump_set_enabled` and `set_schedule_enabled` are **independent, uncoupled**
writes — each does exactly what it says and never touches the other. The pump's
*behavior*, however, couples them (bench-verified, motor RPM as ground truth):

> the motor runs only when the run state is **on** (operation mode `AUTO`,
> `pump_set_enabled true`) **and** the schedule is disabled (runs continuously)
> **or** a schedule window is currently active (runs only in windows).

Consequences for automations calling these raw services:

- **A stopped pump ignores the schedule.** `set_schedule_enabled 1` while the
  run state is `STOP` (`pump_set_enabled false`) leaves a *dead* schedule: the
  enable flag is set, but the pump stays idle through every window. To run on
  schedule the pump must be `AUTO` — also call `pump_set_enabled true`.
  The services stay uncoupled, but this end *state* does not persist: the
  component detects `STOP` + schedule-on with its periodic state poll and
  converges it to `AUTO` + schedule-on (attempts spaced at least five minutes
  apart; suppressed while a vacation covers the current time). That repair
  fires its own `write_settled` event with `origin: "internal"` and
  `op_id: "auto:dead-schedule-repair"` — filter it out if your automation
  reacts to pump-enable writes. Watch for it on
  `sensor.<node_name>_pump_run_state` = `stalled` — see
  [schedule-management.md](schedule-management.md#the-stalled-schedule-and-how-it-repairs-itself).
  To hold the pump off from an automation, clear the schedule flag too
  (`pump_set_state: off`) rather than stopping the pump under an enabled schedule.
- **Started + schedule disabled** → runs continuously (its control mode, 24/7).
- **Started + schedule enabled** → runs only inside windows, idle between them.

The `Engage Pump` and `Schedule Enabled` **entities** hide this by enforcing a
coupled three-state model (Off / Engaged / Scheduled) — see
[schedule-management.md](schedule-management.md#run-state-and-the-schedule).

**For automations, use `pump_set_state`** — the programmatic equivalent, which
gives you the same safety in one call:

| `state` | Result | Legal? |
| --- | --- | --- |
| `off` | `STOP` | pump stopped |
| `engaged` | `AUTO` + schedule off | runs continuously (motor behavior is mode-dependent) |
| `scheduled` | `AUTO` + schedule on | runs only inside schedule windows |

It writes only the flags that differ (a request already satisfied still fires a
terminal event), forces `AUTO` when you ask for `scheduled` (so you can't create
the dead schedule), and orders the writes to avoid a transient `STOP`+schedule
state. The settle event reports the actual `enabled` / `schedule_enabled` flags
and the resulting `state`, so a partial failure surfaces the real end state.

```yaml
service: esphome.hwr_pump_pump_set_state
data:
  state: engaged        # off | engaged | scheduled
  op_id: "run-now"
```

Because `pump_set_state` composes two underlying flag writes, those surface as
their own `op_id: ""` settle events (exactly like an entity toggle); your
`op_id` still gets **exactly one** terminal event with the aggregate result.

The raw `pump_set_enabled` / `set_schedule_enabled` services **stay** as escape
hatches for when you deliberately want to write a single flag without the
coupling.

### Schedules

These keep the names and single `data`-string formats of the original
schedule-editor services (existing automations that pass only `data` keep
working); `op_id` is a new optional argument.

| Service | `data` format | Description |
| --- | --- | --- |
| `set_schedule_entry` | `layer,day,start_h,start_m,end_h,end_m` | Set one recurring entry (day `0`=Monday…`6`=Sunday, layer `0`–`4`) |
| `clear_schedule_entry` | `layer,day` | Clear one recurring entry |
| `set_schedule_enabled` | `0` or `1` | Enable/disable the weekly schedule |
| `refresh_schedule` | *(no data)* | Re-read all layers and refresh the display |
| `set_single_event` | `begin_ts,end_ts` (epoch seconds) | One-time run; a free slot is chosen and echoed in the event |
| `clear_single_event` | `slot` | Clear one single-event slot |
| `refresh_single_events` | *(no data)* | Re-read all single-event slots |
| `upload_schedule` | RFC-005 v1 payload (below) | **Bulk full-state upload** of the entire 7×5 grid in one call |

Schedule writes are **verified**: after the write and commit, the component
reads the schedule back from the pump and compares before reporting. (They
previously reported success unconditionally.)

### Bulk upload (`upload_schedule`)

One service call replaces the 35-clear + N-set sequence. The `data` payload
expresses the **entire desired grid** — any `(layer, day)` cell absent from
the payload is cleared — making the operation idempotent and safe to retry:

```
data     := "v1," enabled ( ";" entry )*
enabled  := "0" | "1" | "-"          ("-" = leave schedule-enabled untouched)
entry    := layer "," day "," sh "," sm "," eh "," em
```

Example — three entries, schedule enabled:

```yaml
service: esphome.hwr_pump_upload_schedule
data:
  data: "v1,1;0,0,6,54,7,0;0,1,7,24,7,30;1,0,17,54,18,0"
  op_id: "sched-2026-07-20"
```

Per layer the component performs a mandatory fresh read, **skips layers whose
image already matches** (zero BLE writes on a no-change re-upload), writes
changed layers as one 42-byte whole-layer frame each, commits, settles, and
readback-verifies. Terminal statuses: `accepted` (all confirmed; detail
`no-op` when everything was skipped), `partial` (mixed — the event carries
`layers_written` / `layers_skipped`), `rejected`, `timeout`, `superseded`.
Watchdog budget: 150 s.

### Schedule hash (sync verification)

The `schedule_hash` text sensor publishes a canonical FNV-1a-64 hash
(`v1:<16hex>`) of the full cached grid + enabled flag, recomputed after every
settled schedule operation ("unknown" until all five layers and the schedule
state are cached). External schedulers compare it against the hash of their
desired schedule to decide whether reprogramming is needed — see RFC-005 in
the dhw-sensor-apps repo for the algorithm and cross-language golden vectors.

### Full-grid read-back (`schedule_layer_0..4` sensors)

Five per-layer text sensors publish each layer's compact JSON (`[start_min,
end_min]` per enabled day, `0` otherwise — each always under HA's 255-char
state cap).  Together with
`schedule_hash` they give an external scheduler a race-free cold-start
recovery path: reconstruct the grid from the five sensors, verify against
the hash, then upload safely (dhw-sensor-apps issue #7).  Sensors report
`unknown` until their layer is cached and are republished after every
settled schedule operation.

## The `write_settled` event

Every service call — and every entity write — ends in exactly one event:

```yaml
event_type: esphome.alpha_hwr_write_settled
data:
  op_id: "restore-speed-1"   # the id you passed; may be empty
  command: "set_setpoint"    # which write this was
  status: "clamped"          # accepted | clamped | rejected | invalid | timeout | superseded
  detail: "pump stored 1650" # short reason when relevant
  origin: "service"          # service (API call) | entity (dashboard write)
                             #   | internal (the component's own self-repair)
  node: "recirc-controller"  # this controller's ESPHome node name
  seq: "42"                  # submission-order sequence number (per boot and per controller)
  # the command's settled values:
  mode: "constant_speed"
  value: "1650"
  enabled: "true"
  # plus an echo of what was requested:
  requested_value: "1500"
```

> **All event values are strings** (`"1650"`, `"true"`) — the ESPHome
> event API sends string maps. Convert in your client if you need numbers.

`node` is this controller's ESPHome node name — `App.get_name()`, normally the
`name:` from its YAML (with the MAC-address suffix appended when
`name_add_mac_suffix` is enabled). It is closely related to the token that
prefixes its service calls, `esphome.<node_name>_...`, but not always
byte-identical: Home Assistant slugifies the name for service registration, so
a hyphenated node like `recirc-controller` appears here verbatim yet becomes
`recirc_controller` in `esphome.recirc_controller_pump_set_enabled`. Use `node`
to attribute an event to a controller when several report to the same event
bus: it's stable and human-readable, unlike Home Assistant's `device_id`,
which is opaque and regenerated if the device is removed and re-added. Because
`seq` is per boot **and per controller**, `node` + `seq` uniquely *identifies*
an event within a multi-controller install (each controller keeps its own seq
counter, so seq values from different nodes are not comparable across them).

Statuses:

- **`accepted`** — the pump confirmed the requested value.
- **`clamped`** — the pump stored a *different* value (e.g. 1500 RPM clamped
  to 1650). The event carries the stored value. Clamping can also come from
  installer limits configured in the Grundfos GO app (pipe size, maximum
  flow), so clamps at unexpected values usually reflect the pump's own
  configuration rather than a protocol limit.
- **`rejected`** — the pump or its state refused: it kept its old value,
  nacked the command, or a precondition could not be read (pump not
  synchronized, schedule overview unreadable). `detail` says why. A retry
  may succeed once conditions change.
- **`invalid`** — the request itself is malformed or out of range (bad
  value, unknown mode, unparsable `data` string). Decided before any wire
  write, deterministic, never worth a retry.
- **`timeout`** — no pump confirmation within the operation's budget, or the
  BLE link dropped mid-operation (`detail: "disconnected"`).
- **`superseded`** — a newer write to the same value replaced this one while
  it was still queued (last write wins). The superseding write still gets its
  own terminal event. Supersede is per-value: e.g. only a second setpoint
  write to the *same mode* supersedes a queued one; setpoints for different
  modes are independent values and both run.

Settled-value fields per command: `mode`/`value`/`enabled` for the control
commands; `temp_min`/`temp_max`/`autoadapt`; `on_minutes`/`off_minutes`/`flow`
(all three are always reported from the readback on confirmed cycle settles,
including fields the request kept — a keep-everything-but-one write doubles
as a read of the others); `layer`/`day`/`day_name`/`begin`/`end`/`enabled`
for schedule entries; `slot`/`begin_ts`/`end_ts`/`enabled` for single events;
`event_count` for the refreshes. For `accepted`/`clamped`/`rejected` these
carry what the pump **actually holds** (from the readback), not what was
requested. The original request is echoed alongside in `requested_*` fields
(`requested_mode`, `requested_value`, `requested_temp_min`/`_max`,
`requested_on_minutes`/`_off_minutes`, `requested_flow`,
`requested_begin`/`_end`), so the event is self-contained for logging and
retry decisions. Kept (`0`-sentinel) cycle fields are omitted from the
`requested_*` echo.

Event ordering note: settle events for `superseded` operations fire at
submission time, so they can arrive **before** the terminal events of
operations submitted earlier. Use the `seq` field when reconstructing
submission order from logs.

## Writing a client

The pattern is: call, wait for your `op_id`, check `status`. In pyscript:

```python
op_id = "restore-speed-1"
esphome.hwr_pump_pump_set_setpoint(mode="constant_speed", value=2500, op_id=op_id)

result = task.wait_until(
    event_trigger=["esphome.alpha_hwr_write_settled", f"op_id == '{op_id}'"],
    timeout=30,
)
if result["status"] == "clamped":
    log.warning(f"speed clamped to {result['value']}")
```

As a Home Assistant automation:

```yaml
automation:
  - alias: "Set speed and alert on clamp"
    trigger: ...
    action:
      - service: esphome.hwr_pump_pump_set_setpoint
        data: {mode: "constant_speed", value: 2500, op_id: "auto-speed"}
      - wait_for_trigger:
          - platform: event
            event_type: esphome.alpha_hwr_write_settled
            event_data: {op_id: "auto-speed"}
        timeout: 30
      - condition: template
        value_template: "{{ wait.trigger.event.data.status != 'accepted' }}"
      - service: notify.notify
        data:
          message: "Pump write {{ wait.trigger.event.data.status }}: {{ wait.trigger.event.data.detail }}"
```

Sequenced writes need no delays between them: calls queue and run strictly one
at a time, each building its wire frames from the arguments you passed — never
from a possibly-stale cache — so a later write can never fold a stale value
into an earlier one.

Concurrent writers behave sanely: last write wins per value, operations never
interleave mid-sequence, and each client matches its own results by `op_id`.

## Notes

- A client that doesn't need confirmation can omit `op_id` and ignore the
  events entirely.
- Typical settle times: a couple of seconds for control writes, a few seconds
  for schedule writes (which include a verify read of the pump's flash-backed
  schedule). Budget your event wait generously (30 s covers everything except
  `refresh_single_events`, which scans up to 35 slots and can take longer on
  a quiet link).
- The pump's own protocol quirks (an ACK window that sometimes closes without
  a matchable ACK even on success) are absorbed by the verify readbacks — a
  write is judged by what the pump reports holding, not by the ACK.
