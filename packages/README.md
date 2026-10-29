# ESPHome ALPHA HWR Packages

The reusable YAML packages for the `alpha_hwr` and `dhw_demand` components,
and the one place that lists what each package declares. The README has the
quick start and the recipes; `docs/configuration.md` has the options behind
each entity; `examples/` has complete, CI-validated configs.

## Available Packages

### `alpha_hwr.yaml` - The pump
The BLE link plus every telemetry and diagnostic entity the component exposes,
and the schedule read-back sensors. The node pairs with the pump on first
connection, while the pump is in Bluetooth pairing mode, and keeps the bond in
NVS.

**Sensors included:**
- Flow Rate (m³/h), Head (m), Head Rate, Motor Speed (RPM), Power (W)
- AC Voltage (V), DC Voltage (V), Motor Current (A)
- Inlet Pressure (bar)
- Water, PCB and Control Box Temperature (°C)
- Active Alarms and Warnings, Pump Run State, Control Mode, Pump Flow Limiter
  and Pump Flow Limited
- Pump Ready, Pairing Status, Pump Link Status / Fault / Recycles / Longest Gap
- Schedule Layer 0-4, Single Events, Vacation, Schedule Hash, Schedule Stalled
- Start Count, Operating Hours, Clock Drift, Last Clock Sync, Pump Clock DST
- Event Log, History Trends and Cycle Timestamps — declared `internal: true`,
  so they are read and logged but not sent to Home Assistant. To export one,
  override it with `internal: false` (a `name:` override alone leaves the
  package's `internal: true` in place)
- Device info (serial, software/hardware/BLE/product version, product name),
  heap, reset reason, component version and build

Also the **Suspend Pump Link** switch, for handing the pump to the Grundfos GO
app without powering the node down.

**Usage:**
```yaml
substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr.yaml@main

esphome:
  name: my-hwr-pump
# ... rest of your config
```

Pair the pump on the first connection: see the README's
[Pairing](../README.md#pairing) section.

`alpha_hwr_pairing.yaml` is this package's old name. It still loads it in
this release and is removed in the next one.

---

### `alpha_hwr_controls.yaml` - Everything you drive from Home Assistant
Layers on `alpha_hwr.yaml`:
- Engage Pump, Remote Mode, Schedule Enabled, Temperature AutoAdapt and the
  flow-limit switches
- Control mode select
- Setpoint numbers for every control mode (Constant Pressure / Speed / Flow,
  Proportional Pressure), Temperature Range Min/Max, Cycle Time ON/OFF and
  Cycle Flow, Max/Min Flow Limit
- Restart HWR Pump and Read Pump Clock buttons, Pump Motor Active indicator
- The schedule editor helpers the Lovelace schedule card uses (day/layer
  selects, time and date numbers, save/clear/vacation buttons). They are
  `internal: true`, so they add nothing to the entity list unless a dashboard
  references them. The schedule services themselves are registered by the
  component and exist either way.

---

### `dhw_demand_detector.yaml` - DHW Demand Detection
Declares the `dhw_demand` component wired to Home Assistant supplementary
sensors (household flow in GPM, lower tank temperature, DHW charge, and
optionally the water heater's DHW in-use flag). Works standalone without a pump;
wire `motor_speed` and `pump_flow` from `alpha_hwr` to enable pump-on
detection. See `docs/configuration.md` for the full key list.

**Entities declared:** DHW Demand (binary sensor), DHW Detection Confidence,
DHW Demand Level, DHW Session Duration, DHW Detection Method.

---

## Quick Start

In the README: [Quick start](../README.md#quick-start). Complete configs are in
[`examples/`](../examples/).

---

## Customization

You can customize sensor names and add filters by overriding the package:

```yaml
packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr.yaml@main

# Override specific sensor configurations
alpha_hwr:
  flow:
    name: "Basement Pump Flow"
    filters:
      - throttle: 10s  # Only update every 10 seconds
  voltage:
    name: "Line Voltage"
```

Or add additional sensors to the same device:

```yaml
packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr.yaml@main

sensor:
  - platform: wifi_signal
    name: "WiFi Signal"
  
```

To convert flow from m³/h to GPM, give the flow sensor an `id` and copy it —
the packages do not assign one by default:

```yaml
alpha_hwr:
  flow:
    id: flow_rate_sensor

sensor:
  - platform: copy
    source_id: flow_rate_sensor
    name: "Flow (GPM)"
    unit_of_measurement: "GPM"
    filters:
      - multiply: 4.40287  # 1 m³/h = 4.40287 GPM
```

---

## Examples

[`examples/`](../examples/), all validated by CI; the three that load the
packages are release-pinned. The README's
[Examples in this repo](../README.md#examples-in-this-repo) says what each one is
for and how secrets work.

---

## Troubleshooting

### MAC Address Not Found
- Make sure Bluetooth is enabled on the ESP32
- Check that your pump is powered on and within range
- Use `esphome logs` to see BLE scan results

### Pairing Fails
- The pump only offers to pair while it is in Bluetooth pairing mode, and
  getting it there takes more than a button press: see the README's
  [Pairing](../README.md#pairing) section
- Leave `initiate_pairing` at its default of `true` (formerly `enable_pairing`,
  still accepted)
- **Do not** clear the node's bond to retry. A pump that holds a bond for a node
  that lost its own drops every connection and never offers to pair again;
  recovery needs physical access to the pump
- Check logs for pairing error messages

### Sensors Show "Unknown"
- Nothing updates until the pump is paired to the node; see the README's
  [Pairing](../README.md#pairing) section
- Wait 10-30 seconds after connection for first telemetry update

