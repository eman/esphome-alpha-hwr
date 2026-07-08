# Configuration

## alpha_hwr Component

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `ble_client_id` | string | **required** | BLE client ID for pump connection |
| `enable_pairing` | boolean | `false` | Enable BLE pairing for control and enhanced telemetry |
| `reconnect_settle_time` | time | `2s` | Delay after disconnect before reconnecting |
| `control_state_poll_interval` | time | `30s` | Interval for periodic control state polling. Set to `0s` to disable. |

## Examples

### Basic read-only monitoring
```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  control_state_poll_interval: 0s
  flow:
    name: "Flow Rate"
  rpm:
    name: "Motor Speed"
```

### Full control with state polling
```yaml
alpha_hwr:
  ble_client_id: hwr_pump_client
  enable_pairing: true
  control_state_poll_interval: 30s
  flow:
    name: "Flow Rate"
  head:
    name: "Head Pressure"
  rpm:
    name: "Motor Speed"
  voltage:
    name: "AC Voltage"
  current:
    name: "Motor Current"
```

## Control State Polling

Periodically reads pump control state to detect changes from internal schedules, manual button presses, or external apps. Keeps component state synchronized with pump reality.

**Default:** 30 seconds polling  
**Disable:** Set to `0s` if you guarantee exclusive component control  
**Customize:** Use any time interval (e.g., `60s`, `15s`)

Polling is non-blocking and doesn't impact other component operations. Failures are logged but don't break anything.
