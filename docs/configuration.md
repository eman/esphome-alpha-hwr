# ALPHA HWR Component Configuration Guide

This document provides comprehensive guidance for configuring the `alpha_hwr` ESPHome component.

## Table of Contents

1. [Basic Configuration](#basic-configuration)
2. [Configuration Options](#configuration-options)
3. [Common Scenarios](#common-scenarios)
4. [Control State Polling (Issue #54)](#control-state-polling-issue-54)
5. [BLE Pairing and Authentication](#ble-pairing-and-authentication)
6. [Reconnection Behavior](#reconnection-behavior)
7. [Complete Examples](#complete-examples)

## Basic Configuration

The minimum viable configuration requires only a BLE client ID:

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
```

This will enable the component with default settings. To expose telemetry to Home Assistant, add sensor definitions:

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  flow:
    name: "Flow Rate"
  head:
    name: "Head Pressure"
  rpm:
    name: "Motor Speed"
  power:
    name: "Power Consumption"
  temp_media:
    name: "Water Temperature"
```

## Configuration Options

### `ble_client_id` (required)

**Type:** string  
**Default:** none

The ESPHome BLE client component ID to use for pump communication. Must match a defined `ble_client` block in your config.

```yaml
ble_client:
  - mac_address: "AA:BB:CC:DD:EE:FF"
    id: hwr_pump_client

alpha_hwr:
  ble_client_id: hwr_pump_client
```

### `enable_pairing` (optional)

**Type:** boolean  
**Default:** `false`

Enable BLE pairing/bonding with the pump for authenticated access and enhanced telemetry (voltage, current, inlet pressure, additional temperatures, alarms, warnings).

**When to use:**
- You need enhanced telemetry beyond basic flow/head/RPM/power/temp
- You want to use pump control features (start/stop, mode changes, setpoints)
- You want to read/write pump schedules

**When NOT to use:**
- You only need basic telemetry
- You want to minimize BLE overhead
- The pump has not been bonded yet and you want to delay pairing setup

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true
```

**Note:** On first connection with pairing enabled, the ESP32 will initiate BLE pairing with the pump. The pump may prompt for confirmation. Bonding keys are stored in ESP32 NVS flash for automatic reconnection on subsequent reboots.

### `reconnect_settle_time` (optional)

**Type:** time  
**Default:** `2s`

Hold-off delay after BLE disconnect before attempting reconnection. This allows the pump time to fully power up and become ready after a reboot, preventing connection attempts during its initialization window.

**Why this matters:** The Grundfos pump has a ~320-720ms post-boot vulnerability window during which encryption requests will fail with error `0x61` and erase the bond. The default 2-second settle time provides ~2.8x safety margin.

**When to customize:**
- Set to `0s` for immediate reconnection (not recommended if pairing is enabled)
- Increase if you experience repeated connection failures after pump reboots
- Decrease if you want faster reconnection in stable network environments

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  reconnect_settle_time: 2s  # Default (recommended)
```

### `control_state_poll_interval` (optional)

**Type:** time  
**Default:** `30s`

Interval for periodic control state polling to detect out-of-band pump state changes (Issue #54).

**Background:** The pump does not send unsolicited notifications when its state changes due to internal schedules, manual button presses, or external app control. Without polling, the component's cached state diverges from the pump's actual state. Periodic polling re-synchronizes the cache.

**When to use:**
- The pump has internal daily/weekly schedules enabled
- Multiple control sources access the pump (app, manual button, component)
- You want to detect state divergence quickly

**When to disable:**
- You guarantee exclusive component control
- You want to minimize BLE traffic
- The pump has no internal scheduling

**Default behavior:** Polls every 30 seconds (configurable, non-blocking, failures are logged but don't affect other operations).

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  control_state_poll_interval: 30s  # Default (detect changes within 30 seconds)
```

**To disable polling:**

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  control_state_poll_interval: 0s  # Disable polling
```

**To customize polling frequency:**

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  control_state_poll_interval: 60s  # Poll every 60 seconds (less BLE traffic)
```

**Polling details:**
- Polling is scheduled 1000ms after telemetry updates to avoid BLE request collisions
- Polling failures are logged as warnings but don't block other operations
- Polling occurs only if session is authenticated and connected
- Millis() rollover (every ~49 days) is handled automatically

## Common Scenarios

### Scenario 1: Basic Read-Only Monitoring

You want to monitor the pump but not control it, and the pump doesn't have internal scheduling.

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  control_state_poll_interval: 0s  # Disable polling (exclusive control)
  flow:
    name: "Flow Rate"
  head:
    name: "Head Pressure"
  rpm:
    name: "Motor Speed"
  power:
    name: "Power Consumption"
  temp_media:
    name: "Water Temperature"
```

### Scenario 2: Remote Control with Multiple Sources

You want to control the pump from the component AND use the pump's physical button or another app. Internal schedule may also run.

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  enable_pairing: true  # Required for control
  control_state_poll_interval: 30s  # Detect out-of-band changes
  
  # Basic telemetry
  flow:
    name: "Flow Rate"
  head:
    name: "Head Pressure"
  rpm:
    name: "Motor Speed"
  power:
    name: "Power Consumption"
  temp_media:
    name: "Water Temperature"
  
  # Enhanced telemetry (requires pairing)
  voltage:
    name: "AC Voltage"
  current:
    name: "Motor Current"
  inlet_pressure:
    name: "Inlet Pressure"
  temp_pcb:
    name: "PCB Temperature"
  temp_control_box:
    name: "Control Box Temperature"
```

### Scenario 3: Schedule Management with Fast Divergence Detection

You have an active internal pump schedule and want fast detection of state changes.

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 15s  # Poll more frequently for fast detection
  
  # ... sensor definitions ...
```

### Scenario 4: Minimize Network Load (High Latency/Low Bandwidth)

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 120s  # Poll less frequently (2 minutes)
  
  # Include only essential sensors
  flow:
    name: "Flow Rate"
  rpm:
    name: "Motor Speed"
```

## Control State Polling (Issue #54)

### Understanding the Problem

The Grundfos ALPHA HWR pump protocol has a quirk: it does not send unsolicited notifications when the pump state changes due to:

- Internal daily/weekly schedules executing
- Manual button press on the pump itself
- State changes from external apps (third-party integrations)

This means the component's cached state (is the pump running? what mode?) can diverge from the pump's actual state if any of these events occur.

### How Polling Solves It

Periodic polling reads the pump's actual control state via `get_mode()` at regular intervals. If the state has changed, the component's cache is updated, and Home Assistant entities reflect the new state.

### Polling Mechanics

- **Interval:** Configurable (default 30 seconds)
- **Trigger:** Automatic, based on elapsed time since last poll
- **Mechanism:** Non-blocking async callback scheduled via ESPHome's `set_timeout`
- **Collision avoidance:** Scheduled 1000ms after telemetry updates
- **Failure handling:** Logged as warning; subsequent polls retry normally
- **Overhead:** ~8 bytes per poll (minimal BLE traffic)

### When to Disable Polling

Set `control_state_poll_interval: 0s` if:

1. You guarantee **exclusive component control** (no internal scheduling, no manual button, no external app)
2. You want to minimize BLE traffic
3. You're running on a low-bandwidth or high-latency network

### When to Increase Polling Frequency

Set a lower interval (e.g., `15s` instead of `30s`) if:

1. You have an active internal pump schedule with frequent state changes
2. Multiple people/systems are controlling the pump
3. You want fast divergence detection for a critical application

## BLE Pairing and Authentication

### What is BLE Pairing?

BLE pairing is a security handshake that:
- Authenticates both devices
- Generates shared encryption keys
- Enables access to "encrypted" services

Without pairing, only "discoverable" pump telemetry (flow, RPM, temperature) is available. With pairing, enhanced telemetry (voltage, current, inlet pressure) and **control operations** (start/stop, mode changes) become available.

### How to Enable Pairing

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true
```

### First Connection with Pairing

On the first connection after enabling `enable_pairing: true`:

1. ESP32 initiates the BLE pairing handshake
2. The pump may display a pairing confirmation (depends on firmware)
3. Once confirmed, bonding keys are stored in ESP32 NVS flash
4. Subsequent connections use the stored keys automatically

### Checking Pairing Status

If `enable_pairing: true`, the component exposes a `pairing_status` binary sensor:

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true
  pairing_status:
    name: "Pump Pairing Status"
```

This sensor will show:
- `ON` → Pairing successful, encrypted telemetry available
- `OFF` → Not paired or pairing failed

### Troubleshooting Pairing Issues

**Symptom:** Pairing fails, bonds keep erasing

**Cause:** The pump may be rebooting during the pairing window. Each reboot resets its bond table.

**Solution:** Increase `reconnect_settle_time` to give the pump more time to stabilize:

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true
  reconnect_settle_time: 5s  # Increased from default 2s
```

**Symptom:** Pairing works initially, then "Encryption not supported" errors

**Cause:** Bonds expired or corrupted in ESP32 NVS.

**Solution:** 
1. Clear NVS on the ESP32 via ESPHome's web interface (Settings → Clear NVS)
2. Reflash and allow pairing to re-establish

## Reconnection Behavior

### Connection Lifecycle

1. **Initial Connection:** ESP32 scans, discovers pump, connects
2. **After Disconnect:** Wait `reconnect_settle_time` (default 2s)
3. **Reconnection Attempt:** ESP32 tries to reconnect
4. **Session Recovery:** If previously paired, reconnection is fast (~100ms after settle window)

### Customize Reconnection Timing

```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  reconnect_settle_time: 2s  # Wait 2 seconds after disconnect before reconnecting
```

**Recommended values:**
- `0s`: Immediate reconnection (use only if pump has proven stability and no pairing)
- `1s`: Fast reconnection (suitable for stable networks)
- `2s`: Balanced (default, covers pump's post-boot vulnerability window)
- `5s`: Conservative (for unstable networks or frequent pump reboots)

## Complete Examples

### Example 1: Minimal Configuration (Read-Only)

```yaml
esphome:
  name: hwr-pump
  friendly_name: HWR Pump

substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_base.yaml@v0.8.0

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

### Example 2: Full Configuration (Control + Schedule + Multi-Source Detection)

```yaml
esphome:
  name: hwr-pump
  friendly_name: HWR Pump

substitutions:
  mac_address: "AA:BB:CC:DD:EE:FF"

packages:
  alpha_hwr: github://eman/esphome-alpha-hwr/packages/alpha_hwr_pairing.yaml@v0.8.0
  controls: github://eman/esphome-alpha-hwr/packages/alpha_hwr_controls.yaml@v0.8.0

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

In the package file (or override in main config):

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 30s  # Detect out-of-band changes from schedule/button
  reconnect_settle_time: 2s
  # ... sensor definitions ...
```

### Example 3: High-Frequency Monitoring (Dashboard with Dashboard)

For critical applications where you need immediate state visibility:

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 10s  # Check every 10 seconds
  reconnect_settle_time: 2s
  
  # All available sensors
  flow:
    name: "Flow Rate"
    unit_of_measurement: "m³/h"
  head:
    name: "Head Pressure"
    unit_of_measurement: "kPa"
  rpm:
    name: "Motor Speed"
    unit_of_measurement: "RPM"
  power:
    name: "Power Consumption"
    unit_of_measurement: "W"
  temp_media:
    name: "Water Temperature"
    unit_of_measurement: "°C"
  voltage:
    name: "AC Voltage"
    unit_of_measurement: "V"
  current:
    name: "Motor Current"
    unit_of_measurement: "A"
  inlet_pressure:
    name: "Inlet Pressure"
    unit_of_measurement: "bar"
  temp_pcb:
    name: "PCB Temperature"
    unit_of_measurement: "°C"
  temp_control_box:
    name: "Control Box Temperature"
    unit_of_measurement: "°C"
```

### Example 4: Efficiency-Optimized (Minimal BLE Traffic)

For battery-powered or bandwidth-constrained setups:

```yaml
alpha_hwr:
  id: pump
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 120s  # Poll every 2 minutes
  reconnect_settle_time: 2s
  
  # Essential sensors only
  flow:
    name: "Flow Rate"
  rpm:
    name: "Motor Speed"
  power:
    name: "Power Consumption"
```

## Troubleshooting

### Component doesn't connect to pump

1. Verify `ble_client_id` matches a defined BLE client
2. Check that the MAC address in the BLE client matches your pump
3. Verify the pump is powered on and not already connected to another device
4. Check ESP32 logs: `esphome logs hwr-pump.yaml`

### Telemetry stops updating

1. Check WiFi/BLE connection
2. Verify BLE client is still connected (check logs)
3. Increase `reconnect_settle_time` if frequent disconnects
4. Restart the component via Home Assistant

### Pairing fails

1. Check logs for encryption errors
2. Increase `reconnect_settle_time` to 5s
3. Clear NVS and re-pair (Settings → Clear NVS in ESPHome web UI)

### Control commands time out

1. Ensure `enable_pairing: true`
2. Check that pairing is successful (pairing_status sensor should be `ON`)
3. Verify pump is not already executing another command
4. Increase command timeout in code if necessary

### Polling doesn't detect out-of-band changes

1. Verify `control_state_poll_interval` is not `0s`
2. Check logs for "Polling control state" messages (should appear every N seconds)
3. Manually change pump state (press button or trigger schedule) and verify detection
4. Increase log level to DEBUG for more details: `logger: level: DEBUG`

## Related Documentation

- [Schedule Management](schedule-management.md) — Detailed guide for reading and writing pump schedules
- [Architecture](architecture.md) — System design and layered component structure
- [ESPHome BLE Client](https://esphome.io/components/ble_client/) — ESPHome official documentation
- [Protocol Reference](https://eman.github.io/alpha-hwr/reimplementation/) — Detailed GENI protocol specification
