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
| `packages/alpha_hwr.yaml` | The pump: BLE link, full telemetry, diagnostics and schedule read-back | Pair the pump with the node on first use; see [Pairing](#pairing) |
| `packages/alpha_hwr_controls.yaml` | Everything you drive from Home Assistant: switches, mode select, setpoints, flow limiters, and the hidden helper entities the Lovelace schedule card uses | Layers on `alpha_hwr.yaml` |
| `packages/dhw_demand_detector.yaml` | The `dhw_demand` component wired to household flow, tank temperature and DHW charge sensors from Home Assistant | Works with or without the pump; see the two `dhw_demand` recipes below |

Two layers for the pump, one for the detector. `alpha_hwr_pairing.yaml` is the
pump package's old name and still loads it, for one release. What each package
declares, entity by entity, is in [`packages/README.md`](packages/README.md);
the options behind them are in [`docs/configuration.md`](docs/configuration.md).

The pump package also sets `logger: level: INFO` and exposes node-health
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
- The pump paired with the node, as with any Bluetooth device. The node pairs
  on first connection; see [Pairing](#pairing) for putting the pump in pairing
  mode
- `api:` enabled if you want Home Assistant services/entities
- `framework.type: esp-idf` is strongly recommended for BLE-based ALPHA HWR
  nodes

## Quick start

The package URLs are meant to be used from another ESPHome project; the package
files pull in the component source themselves. This is the pump with the
control UI. Drop the `alpha_hwr_controls` line for telemetry only.

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
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr.yaml@main
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

Then pair the pump on the first connection: see [Pairing](#pairing). The same
recipe, release-pinned and validated by CI, is
[`examples/hwr-pump-controls-example.yaml`](examples/hwr-pump-controls-example.yaml);
the other examples are listed under [Examples in this repo](#examples-in-this-repo).

### Standalone `dhw_demand`

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

### Combined `alpha_hwr` + `dhw_demand`

If you want pump telemetry plus DHW demand detection, combine the packages and
wire the pump sensors into the detector:

```yaml
# Required: the packages below track @main, so the component source must too.
# Without this, alpha_hwr.yaml's own pin supplies the components while
# the packages supply @main config keys, and validation fails on keys the
# pinned release does not know.
external_components:
  - source: github://eman/esphome-alpha-hwr@main
    components: [alpha_hwr, dhw_demand]

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr.yaml@main
  alpha_hwr_controls: github://eman/esphome-alpha-hwr/packages/alpha_hwr_controls.yaml@main
  dhw_demand: github://eman/esphome-alpha-hwr/packages/dhw_demand_detector.yaml@main

alpha_hwr:
  current:
    id: motor_current_sensor
  # Do not rename the rpm sensor: alpha_hwr_controls.yaml refers to it as
  # `id(motor_speed)`, which is the id alpha_hwr.yaml already assigns.
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
`examples/hwr-pump-dhw-example.yaml` — it is release-pinned and validated by CI,
so it cannot drift out of step with the packages the way an untested snippet can.

## Local development override

When you are working from a local clone and want ESPHome to build the local
component sources instead of the cached GitHub copy, point the packages'
`component_source` substitution at the checkout:

```yaml
substitutions:
  component_source: components   # relative to the config file
```

Override it rather than adding a second `external_components` block. The
packages declare their own, so a second one does not replace it — both sources
are resolved and the pinned repo is still cloned; which one supplies the
component then depends on ESPHome inserting each at the front of
`sys.meta_path` as it goes, so the last one processed wins. That happens to be
the local one today, but nothing in your config says so. The substitution
leaves exactly one source.

Beware the mismatch this exists to avoid: a release-pinned *package* against a
working-tree *component* disagree the moment a config key changes between
releases, and validation fails on a key the pinned side does not know. Point
both at the same place — either both local, or both at the same tag. This is
why `examples/hwr-pump-dhw-example.yaml` says not to add a local block on top of its
tagged packages.

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
(`alpha_hwr_controls.yaml` carries the hidden helper entities the Lovelace
schedule card drives). Home Assistant sees them as:

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

The pump package also publishes schedule read-back text sensors using the same
node-name prefix. ESPHome text sensors surface in Home Assistant under the
`sensor` domain, so these are `sensor.hwr_pump_schedule_layer_0` and
`sensor.hwr_pump_schedule_hash` — there is no `text_sensor.` domain in Home
Assistant.

More detail and automation examples are in
[`docs/schedule-management.md`](docs/schedule-management.md).

## Pairing

The pump pairs with the node the way any Bluetooth device does: once, on the
first connection, and the bond is reused after that. The component's
`initiate_pairing` option defaults to `true`, and the package sets it
explicitly. First-time flow:

1. Put the pump into Bluetooth pairing mode — more involved than one button
   press; see
   [the procedure](docs/configuration.md#initiate_pairing).
2. Flash the ESPHome node.
3. Watch the logs for `BLE authentication complete`. The bond is stored in NVS
   and reconnects reuse it.

The pump accepts **one BLE connection at a time**: while this node is connected,
the Grundfos GO app cannot have the pump. Turn on the **Suspend Pump Link**
switch to hand it over, and off again after; see
[Suspending the BLE link](docs/configuration.md#suspending-the-ble-link).

> **Clearing the node's bond needs physical access to the pump to undo.**
> `ble_client.remove_bond`, an NVS erase, or a re-flash that loses NVS leaves
> the pump bonded to a node that is no longer bonded to it. The pump then drops
> the link on every attempt and never offers to pair again. Recovering means
> standing at the pump and running the
> [re-pairing procedure](docs/configuration.md#initiate_pairing), which needs the
> Grundfos GO app. The node reports this on **Pump Link Fault** as
> `Pump not accepting pairing` once it has happened three connections running.

## Examples in this repo

The examples live in [`examples/`](examples/):

- `hwr-pump-example.yaml` — the pump package on its own: telemetry and
  diagnostics, no control UI
- `hwr-pump-controls-example.yaml` — pump plus the control UI
- `hwr-pump-dhw-example.yaml` — the combined recipe: pump, control UI and
  `dhw_demand`
- `discovery-example.yaml` — a throwaway that logs the MAC address of any ALPHA
  HWR pump in range, for filling in `mac_address`

Each reads WiFi, the API encryption key and the OTA password from `secrets.yaml`,
so start by creating one at the repository root:

```bash
cp secrets-example.yaml secrets.yaml   # then fill in your own values
```

`examples/secrets.yaml` is a committed symlink to that file, because ESPHome
resolves `!secret` beside the config it is loading. The link carries no secret;
`secrets.yaml` itself is gitignored.

Filling it in is not optional. The template's `api_key` is deliberately not a
valid key, its `ap_password` is deliberately too short, and its `ota_password`
is deliberately commented out, so building straight after the copy fails with an
error naming whichever you have not set. That is the point: no example in this
repository carries a credential that would work if flashed, because an
encryption key published in a public repository is not encryption, and a
published OTA password lets anything on your LAN flash the node.

One caveat ESPHome does not warn about: an *empty* `ota_password` is accepted and
silently disables OTA authentication altogether, and an empty `ap_password`
likewise leaves the fallback hotspot open. Set real values rather than blanking
them.

(`tests/ci-compile.yaml` is a CI harness, not a recipe; it omits the API key so
that CI can compile the component before any secrets are seeded.)

## Optional Lovelace schedule card

The schedule card ships in this repo at `dist/alpha-hwr-schedule-card.js`. It is
a separate Home Assistant frontend resource, so ESPHome does not install it —
**install it through HACS** so it stays in step with the firmware.

That matters more than it sounds. The card has drifted out of step with the
firmware twice, and both times silently: a required service argument it never
picked up made every write a no-op, and a display change left the Quick Run list
empty in a way indistinguishable from "no events exist". Neither raised an
error. HACS pins the card to a release and tells you when a newer one exists,
which removes that whole class.

### Prerequisites

- Load `alpha_hwr.yaml`, which publishes the per-layer schedule read-back
  sensors and the single-event text sensor, and `alpha_hwr_controls.yaml`,
  which carries the `Schedule Enabled` switch and the hidden helper entities
  the card drives. The `esphome.<node_name>_*` services the card calls are
  registered by the component itself.

### Install the card with HACS (recommended)

1. In Home Assistant, open **HACS → ⋮ → Custom repositories**.
2. Add `https://github.com/eman/esphome-alpha-hwr` with type **Dashboard**.
3. Find **Alpha HWR Schedule Card** in HACS and install it.
4. Refresh the browser.

HACS registers the dashboard resource itself, so there is no Resources step, and
it raises an update notification when a newer release exists. The version it
installs is the release tag, so the card matches the firmware it was tested
against.

This repo is not (yet) in the HACS default store, which is why the URL has to be
pasted once.

### Install the card by hand (fallback)

For installs not running HACS. Nothing tells you when this copy goes stale, so
check it against the release you are running after each firmware update.

1. Copy `dist/alpha-hwr-schedule-card.js` from this repo into your Home
   Assistant `www` directory.
   - Home Assistant OS / Supervised: usually `/config/www/alpha-hwr-schedule-card.js`
   - Container installs: copy it into the mounted config directory under `www/`
2. In Home Assistant, open **Settings → Dashboards → Resources** and add:
   - **URL**: `/local/alpha-hwr-schedule-card.js`
   - **Resource type**: `JavaScript Module`
3. Refresh the browser, or reload the frontend resources if Home Assistant does
   not pick up the new card immediately.

To check which version a hand-copied card is, look at the header line or the
browser console — the card logs its version once on load. That version is the
alpha_hwr release it shipped with.

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
- The default entity IDs assume the standard names from `alpha_hwr.yaml`
  (`Schedule Layer 0..4`, `Single Events`) and `alpha_hwr_controls.yaml`
  (`Schedule Enabled`). Set `layer_entities` / `enabled_entity` /
  `single_events_entity` only to point at non-default IDs.

## References

- **Configuration**: [docs/configuration.md](docs/configuration.md)
- Protocol docs: <https://eman.github.io/alpha-hwr/reimplementation/>
- Python reference implementation: <https://github.com/eman/alpha-hwr>
- ESPHome BLE client docs: <https://esphome.io/components/ble_client/>
- Architecture notes: [docs/architecture.md](docs/architecture.md)
- Schedule service usage: [docs/schedule-management.md](docs/schedule-management.md)

## License

MIT — see [LICENSE](LICENSE).
