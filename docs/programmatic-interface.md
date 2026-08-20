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

**A service's name is the `command` its settle event reports.** Call
`set_setpoint` and the event comes back with `command: "set_setpoint"`, so you
can correlate a call to its own result by the name you already used, and grep a
log for one string rather than two. The component registers each service *by*
its command name rather than spelling the name twice, so the two cannot drift
apart ([#159](https://github.com/eman/esphome-alpha-hwr/issues/159)).

The one exception is deliberate: `set_vacation` and `clear_vacation` are
compositions over the single-event slots rather than commands of their own, so
they settle as `set_single_event` / `clear_single_event`.

### Pump control

| Service | Arguments | What it writes |
| --- | --- | --- |
| `set_pump_enabled` | `enabled: bool`, `op_id: string` | Run state only, via the pump's unfused Class 3 START/STOP commands — carries no mode and no setpoint at all. A pump-rejected command settles `rejected`; the run state is always confirmed by readback (Class 3 produces no notification of its own). |
| `set_mode` | `mode: string`, `op_id: string` | Control mode only, via the pump's unfused mode-change object — touches neither the run state nor any mode's stored setpoint. |
| `set_setpoint` | `mode: string`, `value: float`, `op_id: string` | **Switches the pump into `mode` AND sets that mode's setpoint** — `mode` is not merely selecting which stored setpoint to edit; after this call the pump is running (or armed) in that mode. This is exactly the pair the pump fuses in one write. Use `set_mode` to switch modes without touching a setpoint. The pump stores an independent setpoint per mode, so setpoint writes to different modes never supersede each other. |
| `set_temperature_range` | `min_c: float`, `max_c: float`, `autoadapt: bool`, `op_id: string` | The temperature-range config object (its own fused write), after switching to temperature-range mode. |
| `set_cycle_times` | `on_minutes: float`, `off_minutes: float`, `flow: float`, `op_id: string` | The cycle-time config object, after switching to cycle-time mode. Minutes are whole, 1–60 (float-typed for platform reasons; fractional values settle `invalid`). `flow` is the flow the pump targets during ON periods, in m³/h (0.1–10.0). **Each field accepts `0` = keep existing**: kept fields are resolved from a fresh read of the pump's stored config (a kept flow is echoed back byte-for-byte, no float round trip), so flow-only or single-period writes are safe. All three at `0` settles `invalid`. |
| `set_pump_state` | `state: string` (`off`\|`engaged`\|`scheduled`), `op_id: string` | **Coupled run-state + schedule selector** — the safe, one-call way to reach a legal state (see [Run state and the schedule](#run-state-and-the-schedule)). Writes only the flags that differ from the current state, in an order that never passes through the dead `STOP`+schedule combo. Unknown `state` settles `invalid`. |

`mode` strings: `constant_pressure`, `proportional_pressure`, `constant_speed`,
`constant_flow`, `auto_adapt_radiator`, `auto_adapt_underfloor`,
`auto_adapt_combined`, `cycle_time`, `temperature_range`.

Setpoint units and ranges: pressure modes in meters (0.5–10.0),
`constant_speed` in RPM (500–4500), `constant_flow` in m³/h (0.1–10.0).

### `set_temperature_range` and `set_cycle_times` settle on the pump, never on the ACK

Both write a configuration object and then read it back, and **the readback is
the verdict** — including when the write went unacknowledged
([#234](https://github.com/eman/esphome-alpha-hwr/issues/234)).

That is worth stating because it used to work the other way. A config write
with no acknowledgement inside its 3 s window settled `rejected` on the spot,
with no readback, and `rejected` asserts the pump did not take the write.
Nothing at that point knew it. A write that landed and whose acknowledgement
was lost — a dropped notification, a reassembly failure, a frame arriving a
moment late — was reported as a failure while the pump held exactly what was
asked for, and an automation retrying on `rejected` would rewrite values that
were already correct.

The acknowledgement could not carry that weight in either direction: it is a
bare Class 10 short ACK with no sequence number and no object echo, so it
cannot be attributed to a particular write — and silence carries less still,
since there is no frame to reason about at all.

Both writes open by **reading their config object**, so the values the verdict
is measured against come from the pump moments earlier rather than from a cache
filled at connect. Without that, an out-of-band edit from the Grundfos GO app
would make a write the pump ignored look like a write the pump adjusted. A
config object that cannot be read settles `rejected` with `write not attempted`,
before anything is sent.

So the three outcomes are decided by what the pump reports holding:

| what the pump did | status | `detail` |
| --- | --- | --- |
| stored the requested values | `accepted` | empty, or the missing-ACK note |
| stored something else | `clamped` | `pump stored …` |
| kept every value it had | `rejected` | `pump kept …` |
| would not answer the readback | `timeout` | `config readback failed` |

A write that was never acknowledged keeps its status from that table and gains
a `config write not acknowledged; …` prefix on `detail`, so the silence is
still reported — it just no longer decides the verdict. It is one of several
cases where an `accepted` settle carries a non-empty `detail` — see the
[`write_settled` statuses](#the-write_settled-event) for the rest.

### Run state and the schedule

`set_pump_enabled` and `set_schedule_enabled` are **independent, uncoupled**
writes — each does exactly what it says and never touches the other. The pump's
*behavior*, however, couples them (bench-verified, motor RPM as ground truth):

> the motor runs only when the run state is **on** (operation mode `AUTO`,
> `set_pump_enabled true`) **and** the schedule is disabled (runs continuously)
> **or** a schedule window is currently active (runs only in windows).

Consequences for automations calling these raw services:

- **A stopped pump ignores the schedule.** `set_schedule_enabled 1` while the
  run state is `STOP` (`set_pump_enabled false`) leaves a *dead* schedule: the
  enable flag is set, but the pump stays idle through every window. To run on
  schedule the pump must be `AUTO` — also call `set_pump_enabled true`.
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
  (`set_pump_state: off`) rather than stopping the pump under an enabled schedule.
- **Started + schedule disabled** → runs continuously (its control mode, 24/7).
- **Started + schedule enabled** → runs only inside windows, idle between them.

The `Engage Pump` and `Schedule Enabled` **entities** hide this by enforcing a
coupled three-state model (Off / Engaged / Scheduled) — see
[schedule-management.md](schedule-management.md#run-state-and-the-schedule).

**For automations, use `set_pump_state`** — the programmatic equivalent, which
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
service: esphome.hwr_pump_set_pump_state
data:
  state: engaged        # off | engaged | scheduled
  op_id: "run-now"
```

Because `set_pump_state` composes two underlying flag writes, those surface as
their own `op_id: ""` settle events (exactly like an entity toggle); your
`op_id` still gets **exactly one** terminal event with the aggregate result.

The raw `set_pump_enabled` / `set_schedule_enabled` services **stay** as escape
hatches for when you deliberately want to write a single flag without the
coupling.

### Remote Mode has no service, but does emit the event

The Remote Mode switch (Remote/Digital vs Local/Panel control source) is an
entity-only write — there is no `set_remote_mode` service yet. It still
runs through the same operation layer as everything else, so toggling it emits
a `set_remote_mode` settle event with `op_id: ""` and `origin: "entity"`, and
it is confirmed from a readback of the pump's `control_source` rather than
from the command ACK: a pump that acks the command and then stays on Local
settles `rejected`, not `accepted`.

### The pump clock has no service either, and its event is `internal`

The pump's real-time clock drives its weekly schedule, so the node syncs it to
its own SNTP time: on the first poll after the link is fully synchronized, and
once a day after that. Nobody asks for either, so both carry
`origin: "internal"` and `op_id: "clock_sync"`; there is no `set_clock` service
to call.

It settles like every other write: the node writes Object 94, reads the pump's
clock back, and compares it against the node's own clock at that moment.
`accepted` means the pump now holds the right time — which is the question worth
answering, so a pump that was already correct also settles `accepted`. A pump
whose clock still disagrees settles `rejected` with the remaining offset in
`detail`; a readback that never decodes settles `timeout`
(`pump clock unreadable`), because a clock we could not read tells us nothing
about whether the pump is wrong.

`clock_offset_s` carries the measured offset in seconds, positive when the pump
runs ahead. It is always absent on `timeout`: a readback that decodes is the
verdict on the spot, `accepted` or `rejected`, so only the undecodable branch
ever retries — and a run that reaches `timeout` is one where nothing ever
decoded and no offset was ever measured.

One asymmetry is worth knowing if you watch these events. The time written into
the frame is built when the operation runs, and the BLE transport sends one
command at a time, so a frame queued behind other traffic reaches the pump late
and leaves it genuinely a few seconds behind. That lag is the node's, not the
pump's, and it cannot exceed the operation's own lifetime — so a pump reading
*behind* by up to that much still settles `accepted`, with the real offset
reported, while a pump reading *ahead* is held to the flat tolerance.

A confirmed sync sets the next attempt a day out. One that does not confirm
retries in 15 minutes rather than a day, so a pump running its schedule off a
wrong clock is not left that way until tomorrow.

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
| `upload_schedule` | v1 payload (below) | **Bulk full-state upload** of the entire 7×5 grid in one call |
| `set_vacation` | `begin_ts,end_ts` (epoch seconds) | A multi-day Stop event overriding the weekly schedule. Settles as `set_single_event` with `event_type: "stop"` — a vacation *is* a single-event slot, not a command of its own |
| `clear_vacation` | *(no data)* | End the active vacation. Settles as `clear_single_event` |

Schedule writes are **verified**: after the write and commit, the component
reads the schedule back from the pump and compares before reporting. (They
previously reported success unconditionally.)

#### Telling a vacation from a one-time run

`set_vacation` and `set_single_event` both settle as `set_single_event`,
because a vacation is a single-event slot rather than a command of its own.
They are nevertheless **opposite instructions** — a vacation writes a *Stop*
event, holding the pump off across the window, while `set_single_event` writes
a *Run*. The settle event distinguishes them:

| `event_type` | Meaning |
| --- | --- |
| `"run"` | a one-time run — the pump operates across the window |
| `"stop"` | a vacation — the pump is held off across the window |

Present on `set_single_event` settles only. A `clear_single_event` disables the
slot whatever it held, so there is no action to report and the key is absent.

The comparison is part of the verification: a pump that acknowledges the write
and stores the *other* kind settles `rejected`, not `accepted`.

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
`no-op` when everything was skipped), `partial` (mixed), `rejected`,
`timeout`, `superseded`. Watchdog budget: 150 s.

Three event fields are specific to this command, and all three are present on
**every** `upload_schedule` settle, not only the partial ones:

| Field | Value |
| --- | --- |
| `layers_written` | comma-separated layer indices actually written, e.g. `"0,2"`; empty when none were |
| `layers_skipped` | comma-separated layer indices whose image already matched, same format |
| `schedule_hash` | the same `v1:<16hex>` value as the `schedule_hash` sensor, recomputed after the operation |

`schedule_hash` is worth reading straight off the event rather than polling
the sensor: it is emitted once any wire work happened — including on a
rejection where every layer failed, because a failed layer has still been read
back and that readback refreshed the cache. It is empty only when the
operation ended before touching the pump.

### Schedule hash (sync verification)

The `schedule_hash` text sensor publishes a canonical FNV-1a-64 hash
(`v1:<16hex>`) of the full cached grid + enabled flag, recomputed after every
settled schedule operation ("unknown" until all five layers and the schedule
state are cached). External schedulers compare it against the hash of their
desired schedule to decide whether reprogramming is needed. The algorithm is
documented in `components/alpha_hwr/schedule_codec.h`, and the golden vectors
in `tests/test_schedule_codec.cpp` are the cross-language contract — an
external scheduler reimplementing the hash must reproduce them exactly.

### Full-grid read-back (`schedule_layer_0..4` sensors)

Five per-layer text sensors publish each layer's compact JSON (`[start_min,
end_min]` per enabled day, `0` otherwise — each always under HA's 255-char
state cap).  Together with
`schedule_hash` they give an external scheduler a race-free cold-start
recovery path: reconstruct the grid from the five sensors, verify against
the hash, then upload safely.  Sensors report
`unknown` until their layer is cached and are republished after every
settled schedule operation.

## The `write_settled` event

Every service call — and every entity write — ends in exactly one event:

```yaml
event_type: esphome.alpha_hwr_write_settled
data:
  op_id: "restore-speed-1"   # the id you passed; may be empty
  command: "set_setpoint"    # which write this was — the service name you called
  status: "clamped"          # accepted | clamped | rejected | invalid | timeout
                             #   | superseded | partial (upload_schedule only)
  detail: "pump stored 1650" # short reason when relevant
  origin: "service"          # service (API call) | entity (dashboard write)
                             #   | internal (the component's own self-repair)
  node: "recirc-controller"  # this controller's ESPHome node name
  seq: "42"                  # submission-order sequence number (per boot and per
                             #   controller); "0" on events the API bridge builds
                             #   itself — see below
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
`recirc_controller` in `esphome.recirc_controller_set_pump_enabled`. Use `node`
to attribute an event to a controller when several report to the same event
bus: it's stable and human-readable, unlike Home Assistant's `device_id`,
which is opaque and regenerated if the device is removed and re-added. Because
`seq` is per boot **and per controller**, `node` + `seq` uniquely *identifies*
an event within a multi-controller install (each controller keeps its own seq
counter, so seq values from different nodes are not comparable across them).

Statuses:

- **`accepted`** — the pump confirmed the requested value. `detail` is usually
  empty. It is not when:
  - a config write the pump stored but never acknowledged says so there (see
    [above](#set_temperature_range-and-set_cycle_times-settle-on-the-pump-never-on-the-ack))
  - `set_single_event` / `set_vacation` reused a slot holding an already-ended
    event — it names the slot and the window it replaced
  - `clear_vacation` found no active vacation (`no active vacation`)
  - `refresh_schedule` reports how many entries it cached (`N entries cached`)
  - `upload_schedule` skipped every layer (`no-op`)
- **`clamped`** — the pump stored a *different* value (e.g. 1500 RPM clamped
  to 1650). The event carries the stored value. Clamping can also come from
  installer limits configured in the Grundfos GO app (pipe size, maximum
  flow), so clamps at unexpected values usually reflect the pump's own
  configuration rather than a protocol limit.
- **`rejected`** — the pump or its state refused: it kept its old value,
  nacked the command, or a precondition could not be read (pump not
  synchronized, schedule overview unreadable). `detail` says why. A retry
  may succeed once conditions change. What it never means is "a reply failed to
  arrive": the two config writes above settle an unacknowledged write by
  readback like any other, so silence alone never produces it.
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
- **`partial`** — `upload_schedule` only: some layers confirmed and some did
  not, or every layer was written but the pump did not take the
  schedule-enabled flag. `layers_written` / `layers_skipped` say which.
  No other command can produce it, but a client switching on `status` needs
  the case.

Settled-value fields per command: `mode`/`value`/`enabled` for the control
commands; `temp_min`/`temp_max`/`autoadapt`; `on_minutes`/`off_minutes`/`flow`
(all three are always reported from the readback on confirmed cycle settles,
including fields the request kept — a keep-everything-but-one write doubles
as a read of the others); `layer`/`day`/`day_name`/`begin`/`end`/`enabled`
for schedule entries; `slot`/`begin_ts`/`end_ts`/`enabled` for single events;
`event_type` for `set_single_event` (see below);
`event_count` for the refreshes; `enabled`/`schedule_enabled`/`state` for
`set_pump_state`; `layers_written`/`layers_skipped`/`schedule_hash` for
`upload_schedule`; `remote_enabled` for `set_remote_mode`;
`clock_offset_s` for `set_clock`.

**`enabled` means two different things**, so check which command you are
looking at: on the control commands it is the pump's run state, and on every
schedule command (`set_schedule_entry`, `clear_schedule_entry`,
`set_schedule_enabled`, `set_single_event`, `clear_single_event`,
`upload_schedule`) it is the *schedule* flag — whether that entry, that slot
or the weekly program is on. `set_pump_state` reports both, under `enabled`
and `schedule_enabled`. `set_remote_mode` deliberately uses a third key,
`remote_enabled`, rather than overloading `enabled` a third time.

For `accepted`/`clamped`/`rejected` these
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

Two kinds of event never reach the operation queue and so carry `seq: "0"`:
a request the API bridge rejects as `invalid` before submitting it (an
unparsable `data` string, an unknown mode or state name), and the aggregate
`set_pump_state` event, which is composed at the bridge from the two flag
writes underneath it. Real sequence numbers start at 1, so `"0"` identifies
them. Correlate these by `op_id`; `node` + `seq` identifies an event uniquely
only among the ones the queue numbered.

## Writing a client

The pattern is: call, wait for your `op_id`, check `status`. In pyscript:

```python
op_id = "restore-speed-1"
esphome.hwr_pump_set_setpoint(mode="constant_speed", value=2500, op_id=op_id)

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
      - service: esphome.hwr_pump_set_setpoint
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
  write is judged by what the pump reports holding, not by the ACK — and since
  the acknowledgement carries no identity at all (GENIbus replies have no
  sequence number and no object echo), that is the only sound way to judge one.
  The transport does what it can underneath: each write waits for its own
  acknowledgement, and a reply owed to a command that already gave up is spent
  settling that debt rather than being handed to the next write (issue #248).
  The cost is
  paid in latency: `set_temperature_range` and `set_cycle_times` each read their
  config object before writing it and wait out the 3 s ACK window before the
  confirm readback starts, so an unacknowledged config write settles several
  seconds later than an acknowledged one — and up to 26 s if its first confirm
  readback is dropped too.
