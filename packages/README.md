# ESPHome ALPHA HWR Packages

This directory contains reusable YAML packages for the Grundfos ALPHA HWR pump component.

## Available Packages

### `alpha_hwr_pairing.yaml` - The pump package
The BLE link plus every telemetry and diagnostic entity the component exposes.
The pump has to be paired to the node: there is no unpaired mode, a peer the
pump has never bonded to gets no connection at all (issue #244). The node pairs
on first connection, while the pump is in Bluetooth pairing mode, and keeps the
bond in NVS. (The file keeps its historical name from when the repo shipped a
second, "unpaired" package; configs reference it by URL.)

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

**Usage:**
```yaml
substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main

esphome:
  name: my-hwr-pump
# ... rest of your config
```

**Note:** put the pump into Bluetooth pairing mode for the first connection; it
takes more than a button press (see `docs/configuration.md`, "Pairing"). The
bond is stored in NVS and reconnects reuse it.

---

### `alpha_hwr_controls.yaml` - Control UI
Recommended control surface. Adds pump enable, remote mode, schedule toggle,
mode select and setpoint controls. Requires `alpha_hwr_pairing.yaml`.

---

### `alpha_hwr_schedule.yaml` - Lighter Schedule/Mode UI
Simpler alternative to `alpha_hwr_controls.yaml`. Avoid combining both unless
you want duplicate controls. Requires `alpha_hwr_pairing.yaml`.

---

### `alpha_hwr_schedule_editor.yaml` - Schedule Editor Helpers
Helper entities for weekly and single-event editing, used by the Lovelace
schedule card. The schedule services themselves are registered by the component,
not by this package. Requires `alpha_hwr_pairing.yaml`.

---

### `dhw_demand_detector.yaml` - DHW Demand Detection
Declares the `dhw_demand` component wired to Home Assistant supplementary
sensors (household flow in GPM, lower tank temperature, DHW charge). Works
standalone without a pump; wire `motor_speed` and `pump_flow` from `alpha_hwr`
to enable pump-on detection. See `docs/configuration.md` for the full key list.

---

## Quick Start

1. **Find your pump's MAC address:**
   - Use ESPHome's Bluetooth scan feature
   - Or use a BLE scanner app (e.g., nRF Connect)

2. **Start from the pump package**, `alpha_hwr_pairing.yaml`, and add
   `alpha_hwr_controls.yaml` if you want the control UI.

3. **Create your device config:**
   ```yaml
   substitutions:
     mac_address: "AA:BB:CC:DD:EE:FF"  # Your pump's MAC
   
   packages:
     alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main
   
   esphome:
     name: hwr-pump-basement
   
   esp32:
     board: esp32-c3-devkitm-1
   
   wifi:
     ssid: !secret wifi_ssid
     password: !secret wifi_password
   
   api:
   ota:
   ```

4. **Flash and enjoy!**
   ```bash
   esphome run my-device.yaml
   ```

---

## Customization

You can customize sensor names and add filters by overriding the package:

```yaml
packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main

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
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@main

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

See the root directory for complete example configurations:
- `hwr-pump-example.yaml` - The pump package on its own
- `hwr-pump-schedule-example.yaml` - Pump with schedule UI and services
- `dhw-demand-example.yaml` - Combined `alpha_hwr` + `dhw_demand`

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

---

## Requirements

- **ESP32** with BLE support (ESP32, ESP32-C3, ESP32-S3)
- **ESPHome 2024.6.0 or newer**
- **ESP-IDF framework** (recommended for BLE stability)

---

## Package Philosophy

These packages follow the **principle of least surprise**:

- `alpha_hwr_pairing.yaml` - Pairs on first connection and keeps the bond;
  the one thing it needs from you is the pump in pairing mode that first time
- The UI and service packages layer on top without touching the link

The packages are designed to be **drop-in replacements** for manually configuring the component, reducing boilerplate and ensuring consistency across deployments.
