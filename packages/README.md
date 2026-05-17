# ESPHome ALPHA HWR Packages

This directory contains reusable YAML packages for the Grundfos ALPHA HWR pump component.

## Available Packages

### `alpha_hwr_base.yaml` - Basic Telemetry
Provides essential pump monitoring without BLE pairing.

**Sensors Included:**
- Flow Rate (m³/h)
- Head Pressure (m)
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
- Grid Voltage (V) - Requires pairing
- Motor Current (A) - Requires pairing
- Converter Temperature (°C)
- PCB Temperature (°C)
- Control Box Temperature (°C)
- Pairing Status (binary sensor)

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
     mac_address: "3C:E0:02:50:98:BF"  # Your pump's MAC
   
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
  
  - platform: template
    name: "Flow (GPM)"
    lambda: |-
      // Convert m³/h to gallons per minute
      if (id(hwr_pump_client).is_connected()) {
        return id(flow_rate).state * 4.4029;
      }
      return 0.0;
    unit_of_measurement: "GPM"
```

---

## Examples

See the root directory for complete example configurations:
- `hwr-pump-example.yaml` - Uses `alpha_hwr_base.yaml`
- `hwr-pairing-example.yaml` - Uses `alpha_hwr_pairing.yaml`

---

## Troubleshooting

### MAC Address Not Found
- Make sure Bluetooth is enabled on the ESP32
- Check that your pump is powered on and within range
- Use `esphome logs` to see BLE scan results

### Pairing Fails
- Ensure `enable_pairing: true` is set
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

- `alpha_hwr_base.yaml` - Works out of the box, no authentication required
- `alpha_hwr_pairing.yaml` - Automatic pairing, no user intervention needed

Both packages are designed to be **drop-in replacements** for manually configuring the component, reducing boilerplate and ensuring consistency across deployments.
