# Schedule Management via Home Assistant

This document covers the pump's schedule services. The services are registered
by the component itself (`components/alpha_hwr/api_bridge.cpp`); the
`packages/alpha_hwr_schedule_editor.yaml` package adds the optional Lovelace
helper entities.

## Service name format

ESPHome prefixes each service with the node name from `esphome.name`.

- Service pattern: `esphome.<node_name>_<service_name>`
- Entity pattern: `<domain>.<node_name>_<entity_name>`
- `-` becomes `_`

If your node is named `hwr-pump`, the service
`set_schedule_entry` becomes `esphome.hwr_pump_set_schedule_entry`.

## Available services

Every service takes its original `data` string plus an optional `op_id`
string. Each call is **verified against the pump** (written, committed, then
read back and compared) and ends in exactly one
`esphome.alpha_hwr_write_settled` event carrying your `op_id` and the settled
values — see [programmatic-interface.md](programmatic-interface.md) for the
event schema and waiting patterns. Calls that used to fail silently (bad data
strings, unreadable schedule state) now settle as `rejected` with a reason.

### Weekly schedule

| Service | Data format | Description |
| --- | --- | --- |
| `esphome.<node_name>_set_schedule_entry` | `layer,day,start_h,start_m,end_h,end_m` | Set a recurring entry |
| `esphome.<node_name>_clear_schedule_entry` | `layer,day` | Clear a recurring entry |
| `esphome.<node_name>_set_schedule_enabled` | `0` or `1` | Enable or disable the weekly schedule |
| `esphome.<node_name>_refresh_schedule` | *(none)* | Re-read the schedule and refresh the text sensor |

- `day`: `0=Monday` … `6=Sunday`
- `layer`: `0` … `4`
- times use 24-hour `hour,minute`

### Single events

| Service | Data format | Description |
| --- | --- | --- |
| `esphome.<node_name>_set_single_event` | `begin_timestamp,end_timestamp` | Schedule a one-time run |
| `esphome.<node_name>_clear_single_event` | `slot_index` | Clear one slot |
| `esphome.<node_name>_refresh_single_events` | *(none)* | Re-read the slots and refresh the text sensor |

- timestamps are Unix epoch seconds
- `set_single_event` picks the first free slot and echoes it in the settle
  event (`slot` field); `rejected` with `"no free single event slots"` when
  full
- a one-time event runs the pump (`Auto` action); it and the weekly windows are
  what "run" means. A vacation is the same object with a `Stop` action — see below

### Vacation

| Service | Data format | Description |
| --- | --- | --- |
| `esphome.<node_name>_set_vacation` | `begin_timestamp,end_timestamp` | Hold the pump **off** for a multi-day period |
| `esphome.<node_name>_clear_vacation` | *(none)* | End the active vacation |

A vacation is a **`Stop`-action single-event** (the same object as one-time
events, sharing the slot pool) that **overrides the weekly schedule** for its
range: while active the pump is held idle regardless of what the weekly grid
says. This matches the Grundfos **Home** app's vacation feature (the GO app does
not expose it). `set_vacation` picks a free slot; `clear_vacation` auto-resolves
whichever slot holds the active `Stop` event (settles `accepted` with no change
if there is no active vacation). The **Vacation** text sensor shows the active
range; the **Single Events** sensor labels each event `(run)` or `(off)`.

For a click-driven UI, `alpha_hwr_schedule_editor.yaml` provides matching
helper entities: four `number` inputs (**Vacation Start Month/Day**,
**Vacation End Month/Day**) plus **Set Vacation** and **Clear Vacation**
buttons. "Set Vacation" holds the pump off from 00:00 of the start day through
23:59 of the end day (current year, whole-day granularity) by calling
`set_vacation`; "Clear Vacation" calls `clear_vacation`. The helpers are
`internal: true` — expose them on a dashboard or reference them from a Lovelace
card. Automations should call the services directly rather than these buttons.

## Run state and the schedule

The weekly schedule does not run the pump on its own — it **gates** an
already-running pump. Bench-verified rule (motor RPM as ground truth):

> the motor runs only when the run state is **on** (operation mode `AUTO`)
> **and** the schedule is disabled (runs continuously) **or** a schedule window
> is currently active (runs only in windows).

So a pump whose run state is **off** (`STOP`) stays idle through every window
even with the schedule enabled — enabling the schedule on a stopped pump does
nothing until the pump is switched to `AUTO`.

The two switches model this as three mutually-exclusive states, matching the
Grundfos GO app (which won't let you start the pump while the schedule is on):

| State | `Run Pump` | `Schedule Enabled` | Behavior |
| --- | --- | --- | --- |
| **Off** | off | off | pump stopped |
| **Run** | **on** | off | runs continuously in its control mode |
| **Scheduled** | off | **on** | runs only inside schedule windows |

- **`Run Pump` on** → run continuously now, and **disable the schedule**.
- **`Schedule Enabled` on** → switch the pump to `AUTO` so the schedule can
  actually run it; `Run Pump` then reads off (the pump is gated, not continuous).
- **`Schedule Enabled` off** → stop the pump.

`Run Pump` reads on only when the pump is running continuously (`AUTO` **and**
schedule off), so the two switches are always mutually exclusive. (Previously a
"Pump Enabled" switch could read on while an enabled schedule held the motor
idle — and a stopped pump with the schedule enabled silently never ran.)

Automations that call the raw `pump_set_enabled` / `set_schedule_enabled`
services bypass this coupling and can set any combination — see
[programmatic-interface.md](programmatic-interface.md#run-state-and-the-schedule).

## Behavior change (v0.11)

Schedule writes previously reported "OK" in the logs unconditionally — even
when the pump discarded the write. They are now verified with a readback, so
a write that doesn't stick settles as `rejected` (and a schedule that can't be
read settles the write as `rejected` *before* anything is sent, rather than
risking a blind read-modify-write). If an automation of yours starts reporting
failures that used to look like successes, the failures were already
happening — they're just visible now.

## Example automations

Replace `<node_name>` below with your ESPHome node name after converting `-`
to `_`.

### Weekday morning schedule

```yaml
automation:
  - alias: "Set weekday pump schedule"
    trigger:
      - platform: homeassistant
        event: start
    action:
      - repeat:
          count: 5
          sequence:
            - service: esphome.<node_name>_set_schedule_entry
              data:
                data: "0,{{ repeat.index - 1 }},6,0,8,0"
```

### Emergency 2-hour run

```yaml
automation:
  - alias: "Emergency pump run"
    trigger:
      - platform: state
        entity_id: input_boolean.emergency_pump
        to: "on"
    action:
      - service: esphome.<node_name>_set_single_event
        data:
          data: "{{ (now().timestamp() | int) + 60 }},{{ (now().timestamp() | int) + 7260 }}"
```

### Solar excess trigger

```yaml
automation:
  - alias: "Solar excess pump run"
    trigger:
      - platform: numeric_state
        entity_id: sensor.solar_excess_power
        above: 500
        for: "00:05:00"
    action:
      - service: esphome.<node_name>_set_single_event
        data:
          data: "{{ (now().timestamp() | int) + 60 }},{{ (now().timestamp() | int) + 3660 }}"
```

## REST API examples

```bash
# Set Monday 06:00-08:00 on layer 0
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"data": "0,0,6,0,8,0"}' \
  https://homeassistant.local:8123/api/services/esphome/<node_name>_set_schedule_entry

# Schedule a one-time run
curl -X POST \
  -H "Authorization: Bearer YOUR_TOKEN" \
  -H "Content-Type: application/json" \
  -d '{"data": "1739664000,1739671200"}' \
  https://homeassistant.local:8123/api/services/esphome/<node_name>_set_single_event
```

## Related entities

| Entity | Description |
| --- | --- |
| `text_sensor.<node_name>_schedule_layer_0..4` | Per-layer schedule read-back JSON (compact, one sensor per layer) |
| `text_sensor.<node_name>_schedule_hash` | Canonical hash of the cached grid, for sync verification |
| `text_sensor.<node_name>_single_events` | Human-readable active single events |
| `text_sensor.<node_name>_vacation` | Active vacation range, or "No vacation" |

## Notes

- The services are registered by the component itself (`api_bridge.cpp`);
  `alpha_hwr_schedule_editor.yaml` adds the optional Lovelace helper entities
  (day/layer selects, time/date `number` inputs, save/clear/vacation buttons);
  `alpha_hwr_pairing.yaml` provides the schedule, single-event, and vacation
  text sensors.
- Single events temporarily override the weekly schedule while active.
- Schedule writes take a few seconds to propagate over BLE.
- Call `refresh_schedule` or `refresh_single_events` after bulk updates if you
  want Home Assistant to refresh the displayed state immediately.
