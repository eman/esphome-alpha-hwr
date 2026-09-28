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
- Active Alarms and Warnings, Run State, Control Mode, Flow Limiter
- Pump Ready, Pairing Status, Pump Link Status / Fault / Recycles / Longest Gap
- Schedule layers, single events, vacation, schedule hash and stall
- Event log, history trends, cycle timestamps, start count, operating hours,
  clock drift and last clock sync
- Device info, heap, reset reason, component version and build

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

**Note:** put the pump into Bluetooth pairing mode for the first connection; it
takes more than a button press (see `docs/configuration.md`, "Pairing"). The
bond is stored in NVS and reconnects reuse it.

`alpha_hwr_pairing.yaml` is this package's old name. It still loads it, and
goes away in the release after next.

---

### `alpha_hwr_controls.yaml` - Everything you drive from Home Assistant
Layers on `alpha_hwr.yaml`:
- Engage Pump, Remote Mode, Schedule Enabled, Temperature AutoAdapt and the
  flow-limit switches
- Control mode select
- Setpoint, temperature-range, cycle-time and flow-limit numbers
- Restart and Read Pump Clock buttons, Pump Motor Active indicator
- The schedule editor helpers the Lovelace schedule card uses (day/layer
  selects, time and date numbers, save/clear/vacation buttons). They are
  `internal: true`, so they add nothing to the entity list unless a dashboard
  references them. The schedule services themselves are registered by the
  component and exist either way.

---

### `dhw_demand_detector.yaml` - DHW Demand Detection
Declares the `dhw_demand` component wired to Home Assistant supplementary
sensors (household flow in GPM, lower tank temperature, DHW charge). Works
standalone without a pump; wire `motor_speed` and `pump_flow` from `alpha_hwr`
to enable pump-on detection. See `docs/configuration.md` for the full key list.

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

[`examples/`](../examples/), each release-pinned and validated by CI. The README's
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
  getting it there takes more than a button press: see `docs/configuration.md`,
  "Pairing"
- Leave `initiate_pairing` at its default of `true` (formerly `enable_pairing`,
  still accepted)
- **Do not** clear the node's bond to retry. A pump that holds a bond for a node
  that lost its own drops every connection and never offers to pair again;
  recovery needs physical access to the pump
- Check logs for pairing error messages

### Sensors Show "Unknown"
- No sensor updates until the pump is paired to the node. A never-paired peer
  is refused at the link layer, and a peer with a stale bond is dropped about
  2 s after connecting (issues #244, #230)
- Voltage/current sensors **require** pairing to be enabled
- Wait 10-30 seconds after connection for first telemetry update

