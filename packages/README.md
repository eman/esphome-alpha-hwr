# ESPHome ALPHA HWR Packages

This directory contains reusable YAML packages for the Grundfos ALPHA HWR pump component.

## Available Packages

### `alpha_hwr_base.yaml` - Basic Telemetry
Provides essential pump monitoring without BLE pairing.

**Sensors Included:**
- Flow Rate (m³/h)
- Head (m)
- Water Temperature (°C)
- Motor Speed (RPM)
- Power Consumption (W)

**Usage:**
```yaml
substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_base.yaml@main
```

---

### `alpha_hwr_pairing.yaml` - Enhanced Telemetry with Pairing
Provides complete pump diagnostics with BLE pairing/bonding enabled.

**Additional Sensors (vs. base):**
- AC Voltage (V) - Requires pairing
- DC Voltage (V) - Requires pairing
- Motor Current (A) - Requires pairing
- Inlet Pressure (bar)
- PCB Temperature (°C)
- Control Box Temperature (°C)
- Pairing Status (binary sensor)

Also adds device info, history, event log and statistics sensors, the control
mode text sensor, and the schedule/single-event/vacation read-back sensors.

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

**Note:** On first connection, the pump will automatically pair/bond with your ESP32. Bonding keys are stored in NVS flash for automatic reconnection on subsequent boots.

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

2. **Choose a package:**
   - Use `alpha_hwr_base.yaml` for basic monitoring
   - Use `alpha_hwr_pairing.yaml` for full diagnostics

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
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_base.yaml@main

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
- `hwr-pump-example.yaml` - Uses `alpha_hwr_base.yaml`
- `hwr-pairing-example.yaml` - Uses `alpha_hwr_pairing.yaml`
- `hwr-pump-schedule-example.yaml` - Paired pump with schedule UI and services
- `dhw-demand-example.yaml` - Combined `alpha_hwr` + `dhw_demand`

---

## Troubleshooting

### MAC Address Not Found
- Make sure Bluetooth is enabled on the ESP32
- Check that your pump is powered on and within range
- Use `esphome logs` to see BLE scan results

### Pairing Fails
- Ensure `initiate_pairing: true` is set (formerly `enable_pairing`, still accepted)
- Try erasing NVS flash: `esphome run --erase-nvs`
- Check logs for pairing error messages

### Sensors Show "Unknown"
- Basic sensors (flow, temp, RPM) work without pairing
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

- `alpha_hwr_base.yaml` - Works out of the box, no BLE pairing required
- `alpha_hwr_pairing.yaml` - Automatic pairing, no user intervention needed

Both packages are designed to be **drop-in replacements** for manually configuring the component, reducing boilerplate and ensuring consistency across deployments.
