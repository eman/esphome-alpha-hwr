# ESPHome ALPHA HWR + DHW Demand

ESPHome repository for two custom components:

- `alpha_hwr` — BLE telemetry and control for the Grundfos ALPHA HWR pump
- `dhw_demand` — on-device DHW demand detection using pump telemetry and/or
  Home Assistant sensors

The repo also ships reusable package YAML so an external ESPHome config can pull
in the component stack directly from GitHub.

## What each package does

| Package | Purpose | Notes |
| --- | --- | --- |
| `packages/alpha_hwr_base.yaml` | Basic ALPHA HWR telemetry without BLE pairing | Good starting point for read-only monitoring |
| `packages/alpha_hwr_pairing.yaml` | Full telemetry, diagnostics, schedules, and paired BLE access | Required for controls and schedule editing |
| `packages/alpha_hwr_controls.yaml` | Recommended control UI | Adds pump enable, remote mode, schedule toggle, mode select, and setpoint controls |
| `packages/alpha_hwr_schedule.yaml` | Lighter schedule/remote/mode UI | Simpler alternative to `alpha_hwr_controls.yaml`; avoid combining both unless you want duplicate controls |
| `packages/alpha_hwr_schedule_editor.yaml` | ESPHome services and helper entities for weekly/single-event editing | Pair with `alpha_hwr_pairing.yaml` |
| `packages/dhw_demand_detector.yaml` | DHW detector outputs plus Home Assistant supplementary sensors | Works standalone or alongside `alpha_hwr` |

Both pump packages also set `logger: level: INFO` and expose node-health
diagnostics (`Free Heap`, `Min Free Heap`, `Largest Free Block`, `Heap
Fragmentation`, `Reset Reason`). Log lines and state changes are API frames
delivered to every connected subscriber, so DEBUG is opt-in — put your own
`logger:` block in your config to override the package. See
[Node Health Diagnostics](docs/configuration.md#node-health-diagnostics).

## Requirements

- **alpha_hwr**: ESP32-class board with BLE (`ESP32`, `ESP32-C3`, `ESP32-S3`)
- **dhw_demand standalone**: any ESPHome-capable board if you only use Home
  Assistant-fed sensors
- `substitutions.mac_address` for the pump packages
- `api:` enabled if you want Home Assistant services/entities
- `framework.type: esp-idf` is strongly recommended for BLE-based ALPHA HWR
  nodes

## Basic vs paired `alpha_hwr`

| Feature | `alpha_hwr_base.yaml` | `alpha_hwr_pairing.yaml` |
| --- | --- | --- |
| Flow, head, water temperature, RPM, power | Yes | Yes |
| AC/DC voltage, motor current | No | Yes |
| Inlet pressure | No | Yes |
| PCB and control-box temperatures | No | Yes |
| Pairing status | No | Yes |
| Control mode text sensor | No | Yes |
| Schedule and single-event text sensors | No | Yes |
| Start/stop, remote control, schedule toggle, mode/setpoint UI | No | Add `alpha_hwr_controls.yaml` or `alpha_hwr_schedule.yaml` |
| Device info, history, event log, statistics | No | Yes |

## Using these packages from an external ESPHome config

The package URLs below are meant to be used from another ESPHome project. The
package files already pull the required external components for `alpha_hwr`.

### 1. Basic read-only pump telemetry

```yaml
esphome:
  name: hwr-pump
  friendly_name: HWR Pump

substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

# Required whenever the packages track @main. Each package self-declares an
# `external_components` block pinned to the release it shipped with, so without
# this the component source stays at that tag while the package config moves
# ahead — and any key added since the release is rejected as "an invalid option
# for [alpha_hwr]". ESPHome does not dedupe these blocks; the last merged entry
# wins, so a top-level declaration overrides the package's.
external_components:
  - source: github://eman/esphome-alpha-hwr@main
    components: [alpha_hwr]

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_base.yaml@main

esp32:
  board: esp32-c3-devkitm-1
  variant: esp32c3
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    password: !secret ota_password
```

### 2. Full telemetry plus control UI

```yaml
esphome:
  name: hwr-pump
  friendly_name: HWR Pump

substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

# See §1 — required whenever the packages track @main.
external_components:
  - source: github://eman/esphome-alpha-hwr@main
    components: [alpha_hwr]

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main
  alpha_hwr_controls: github://eman/esphome-alpha-hwr/packages/alpha_hwr_controls.yaml@main

esp32:
  board: esp32-c3-devkitm-1
  variant: esp32c3
  framework:
    type: esp-idf

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
  encryption:
    key: !secret api_key

ota:
  - platform: esphome
    password: !secret ota_password
```

### 3. Add schedule editor services

Add the schedule editor package on top of the paired pump config:

```yaml
# See §1 — required whenever the packages track @main.
external_components:
  - source: github://eman/esphome-alpha-hwr@main
    components: [alpha_hwr]

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main
  alpha_hwr_controls: github://eman/esphome-alpha-hwr/packages/alpha_hwr_controls.yaml@main
  alpha_hwr_schedule_editor: github://eman/esphome-alpha-hwr/packages/alpha_hwr_schedule_editor.yaml@main
```

`alpha_hwr_schedule_editor.yaml` exposes ESPHome services such as
`set_schedule_entry` and `set_single_event`.

### 4. Standalone `dhw_demand`

When you use `dhw_demand` without `alpha_hwr`, declare the component explicitly
with `external_components`. `flow_entity` is a Home Assistant sensor reporting
household flow in GPM:

```yaml
esphome:
  name: dhw-detector
  friendly_name: DHW Detector

substitutions:
  flow_entity: sensor.dhw_flow_rate
  tank_lower_temp_entity: sensor.tank_lower_temperature
  dhw_charge_entity: sensor.dhw_charge

external_components:
  - source: github://eman/esphome-alpha-hwr@main
    components: [dhw_demand]

packages:
  dhw_demand: github://eman/esphome-alpha-hwr/packages/dhw_demand_detector.yaml@main

esp32:
  board: esp32dev

wifi:
  ssid: !secret wifi_ssid
  password: !secret wifi_password

api:
ota:
  - platform: esphome
```

### 5. Combined `alpha_hwr` + `dhw_demand`

If you want pump telemetry plus DHW demand detection, combine the packages and
wire the pump sensors into the detector:

```yaml
# Required: the packages below track @main, so the component source must too.
# Without this, alpha_hwr_pairing.yaml's own pin supplies the components while
# the packages supply @main config keys, and validation fails on keys the
# pinned release does not know.
external_components:
  - source: github://eman/esphome-alpha-hwr@main
    components: [alpha_hwr, dhw_demand]

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main
  alpha_hwr_controls: github://eman/esphome-alpha-hwr/packages/alpha_hwr_controls.yaml@main
  dhw_demand: github://eman/esphome-alpha-hwr/packages/dhw_demand_detector.yaml@main

alpha_hwr:
  current:
    id: motor_current_sensor
  # Do not rename the rpm sensor: alpha_hwr_controls.yaml refers to it as
  # `id(motor_speed)`, which is the id alpha_hwr_pairing.yaml already assigns.
  flow:
    id: flow_rate_sensor

sensor:
  - platform: copy
    source_id: flow_rate_sensor
    id: dhw_pump_flow_gpm
    internal: true
    unit_of_measurement: "GPM"
    filters:
      - multiply: 4.40287

dhw_demand:
  motor_speed: motor_speed
  motor_current: motor_current_sensor
  pump_flow: dhw_pump_flow_gpm
```

`motor_speed` and `pump_flow` are what make pump-on detection work: while the
pump is running, household demand is measured as `flow − pump_flow`, gated on
`motor_speed`. Without both, the detector still works while the pump is off and
reports `pump_on_uncertain` whenever it is running.

If your water heater exposes a DHW in-use flag, wire it as `dhw_in_use` for a
third detection tier:

```yaml
sensor:
  - platform: homeassistant
    id: dhw_in_use_flag
    entity_id: sensor.your_dhw_in_use  # must report numeric 0/1

dhw_demand:
  dhw_in_use: dhw_in_use_flag
```

`dhw_in_use` is an ESPHome **sensor**, not a binary sensor, and is read as a
float. The Home Assistant entity must therefore report a number — `0`/`1` or
`0.0`/`1.0`. Pointing it at a `binary_sensor`, whose state is the string
`on`/`off`, yields `NaN` at runtime and the tier silently never fires. If your
flag is a `binary_sensor`, map it to a numeric template sensor in Home
Assistant first.

The flag is unusable bare — it fires often and briefly — so it only declares a
pump-on draw after holding continuously for `dhw_in_use_min_seconds` (70 s by
default). It never displaces a stronger tier and only ever adds demand. It is
entirely optional; leave it out and the other two tiers are unaffected.

For a complete working version of this combined recipe, see
`hwr-pump-dhw-example.yaml` — it is release-pinned and validated by CI, so it
cannot drift out of step with the packages the way an untested snippet can. For
the detector without the control UI, see `dhw-demand-example.yaml`.

## Local development override

When you are working from a local clone and want ESPHome to build the local
component sources instead of the cached GitHub copy, add:

```yaml
external_components:
  - source:
      type: local
      path: components
    components: [alpha_hwr, dhw_demand]
```

That is the pattern used in `dhw-demand-example.yaml`.

## Programmatic control (services + `write_settled` event)

For automations, scripts, or any program driving the pump, the component
registers write services (`set_pump_enabled`, `set_pump_state`,
`set_mode`, `set_setpoint`, `set_temperature_range`,
`set_cycle_times`, plus the schedule services below). Every write — service- or
entity-originated — is serialized, verified against a pump readback, and
settles with exactly one `esphome.alpha_hwr_write_settled` event reporting
whether it was `accepted`, `clamped`, `rejected`, `timeout`, or `superseded`,
along with the value the pump actually stored. Pass an `op_id` of your choice
to match results to your own calls — no fixed delays, no internal timing to
know.

Requires `custom_services: true` and `homeassistant_services: true` on the
`api:` component (the shipped packages set both). Full contract and client
examples: [`docs/programmatic-interface.md`](docs/programmatic-interface.md).

## Schedule services and entity names

The schedule services are registered by the component itself
(`alpha_hwr_schedule_editor.yaml` adds the optional Lovelace helper
entities). Home Assistant sees them as:

- `esphome.<node_name>_set_schedule_entry`
- `esphome.<node_name>_clear_schedule_entry`
- `esphome.<node_name>_set_schedule_enabled`
- `esphome.<node_name>_refresh_schedule`
- `esphome.<node_name>_set_single_event`
- `esphome.<node_name>_clear_single_event`
- `esphome.<node_name>_refresh_single_events`
- `esphome.<node_name>_upload_schedule`
- `esphome.<node_name>_set_vacation`
- `esphome.<node_name>_clear_vacation`

`<node_name>` comes from `esphome.name` with `-` converted to `_`. Example:

- `esphome.name: hwr-pump`
- Home Assistant service: `esphome.hwr_pump_set_schedule_entry`

The paired package also publishes schedule read-back text sensors using the same
node-name prefix. ESPHome text sensors surface in Home Assistant under the
`sensor` domain, so these are `sensor.hwr_pump_schedule_layer_0` and
`sensor.hwr_pump_schedule_hash` — there is no `text_sensor.` domain in Home
Assistant.

More detail and automation examples are in
[`docs/schedule-management.md`](docs/schedule-management.md).

## Pairing

`alpha_hwr_pairing.yaml` enables BLE pairing and stores the bond in NVS. Typical
first-time flow:

1. Put the pump into Bluetooth pairing mode.
2. Flash the ESPHome node with the paired package.
3. Watch logs for the authentication/pairing sequence to complete.

After that, reconnects reuse the stored bond.

## Examples in this repo

- `hwr-pump-example.yaml` — basic read-only `alpha_hwr`
- `hwr-pairing-example.yaml` — paired `alpha_hwr`
- `hwr-pump-schedule-example.yaml` — paired pump with schedule UI/services
- `dhw-demand-example.yaml` — paired `alpha_hwr` + `dhw_demand`
- `hwr-pump-dhw-example.yaml` — the §5 combination: paired `alpha_hwr` +
  control UI + `dhw_demand`

## Optional Lovelace schedule card

The schedule card source ships in this repo at
`homeassistant/www/alpha-hwr-schedule-card.js`. It is a separate Home Assistant
frontend resource, so ESPHome does not install it automatically.

### Prerequisites

- Use `alpha_hwr_pairing.yaml` so Home Assistant gets the per-layer schedule
  read-back sensors, the `Schedule Enabled` switch, and the single-event text
  sensor.
- Use `alpha_hwr_schedule_editor.yaml` so Home Assistant gets the
  `esphome.<node_name>_*` services the card calls when you edit schedules.

### Install the card in Home Assistant

1. Copy `homeassistant/www/alpha-hwr-schedule-card.js` from this repo into your
   Home Assistant `www` directory.
   - Home Assistant OS / Supervised: usually `/config/www/alpha-hwr-schedule-card.js`
   - Container installs: copy it into the mounted config directory under `www/`
2. In Home Assistant, open **Settings → Dashboards → Resources** and add:
   - **URL**: `/local/alpha-hwr-schedule-card.js`
   - **Resource type**: `JavaScript Module`
3. Refresh the browser, or reload the frontend resources if Home Assistant does
   not pick up the new card immediately.

### Lovelace example

```yaml
type: custom:alpha-hwr-schedule-card
title: Pump Schedule
device: hwr_pump
```

`device` is the only required option. From it the card derives the per-layer
read-back sensors (`sensor.<device>_schedule_layer_0..4`), the
`Schedule Enabled` switch (`switch.<device>_schedule_enabled`), and the
single-event sensor (`sensor.<device>_single_events`). Override any of them
only if your entity IDs differ from the defaults:

```yaml
type: custom:alpha-hwr-schedule-card
title: Pump Schedule
device: hwr_pump
enabled_entity: switch.hwr_pump_schedule_enabled
single_events_entity: sensor.hwr_pump_single_events
layer_entities:
  - sensor.hwr_pump_schedule_layer_0
  - sensor.hwr_pump_schedule_layer_1
  - sensor.hwr_pump_schedule_layer_2
  - sensor.hwr_pump_schedule_layer_3
  - sensor.hwr_pump_schedule_layer_4
```

### Optional forecast and desired-schedule overlays

The card grid shows what the pump is programmed to do but not why. Two optional
entities add that context, both unset by default — omit them and the card
renders exactly as it did before:

```yaml
type: custom:alpha-hwr-schedule-card
title: Pump Schedule
device: hwr_pump
forecast_entity: sensor.dhw_forecast_weekly_series
desired_entity: sensor.dhw_pump_schedule_series
```

- `forecast_entity` paints a weekly forecast's demand windows as a translucent
  heat strip behind each day row, opacity scaled by peak probability, so you can
  see whether a pre-heat burst lands in front of predicted demand.
- `desired_entity` outlines intervals the scheduler wants but the device is not
  holding, surfacing scheduler-vs-device drift. Intervals that already match are
  drawn as normal blocks rather than ghosted.

Both overlays sit beneath the interactive blocks and are `pointer-events: none`,
so dragging and editing are unaffected.

### Choosing the right names

- `device` must match the ESPHome node-derived service prefix: `esphome.name`
  with `-` converted to `_`. For example, if `esphome.name: hwr-pump`, use
  `device: hwr_pump`.
- The default entity IDs assume the standard names from `alpha_hwr_pairing.yaml`
  (`Schedule Layer 0..4`, `Schedule Enabled`) and
  `alpha_hwr_schedule_editor.yaml`. Set `layer_entities` / `enabled_entity` /
  `single_events_entity` only to point at non-default IDs.

## References

- **Configuration**: [docs/configuration.md](docs/configuration.md)
- Protocol docs: <https://eman.github.io/alpha-hwr/reimplementation/>
- Python reference implementation: <https://github.com/eman/alpha-hwr>
- ESPHome BLE client docs: <https://esphome.io/components/ble_client/>
- Architecture notes: [docs/architecture.md](docs/architecture.md)
- Schedule service usage: [docs/schedule-management.md](docs/schedule-management.md)
