# Additional HWR telemetry: trace audit

The inspected Android snoops were reassembled into complete GENI replies and
filtered by CRC, APDU ACK, object type/version and declared object length.
Values were decoded directly from object bodies, without using request names
from the generated timelines. Some timeline rows pair requests with unrelated
object replies, so their request/object labels alone are not evidence.

| Capture | Valid motor replies | Converter/inverter temperature | Auxiliary temperature | Valid process replies | Differential temperature |
| --- | ---: | --- | --- | ---: | --- |
| `btsnoop_hci.log` | 40 | NaN in all 40 | 0°C in all 40 | 51 | NaN in all 51 |
| `btsnoop_hci_1.log` | 2 | NaN in both | 0°C in both | 61 | NaN in all 61 |

The converter and inverter fields describe power-electronics temperatures,
not water temperature or a measured motor-winding temperature. The profile
calls the third field `aux_temperature` without identifying its physical
sensor. Its constant zero is compatible with a placeholder; it does not
establish a real 0°C measurement.

The generic process object's `differential_temperature` is a temperature
*difference*. The inspected HWR profile does not identify the temperature pair,
and none of these replies supplies a finite measurement. The implementation
exposes the native value rather than synthesizing one from other sensors.
These four diagnostics are disabled by default in the pump package.

DC power is populated: the two running motor replies contain 10.469604 and
10.075836 W, while the stopped replies contain 0 W. Power-on time is populated:
3009.176667–3009.217500 h in the running capture and
3014.236944–3014.257778 h in the other capture. It exceeds operating time because
it includes periods when the motor is stopped.

The app's bundled `geni_profile_52_7.xml` identifies motor object 256, process
object 565 v2 and statistics object 248 v2. Captured motor replies use version
3 with the common 41-byte field layout. Panel settings are 94/2, type 269 v1
(`PinLockOnSettings`), and 94/114, type 919 v1
(`HMIHiddenModeConfiguration`). The observed bodies are `01 00 00` and `01 78`
respectively: settings locked with no PIN bytes, and hidden/display mode enabled
with timeout 120 seconds. The write implementation preserves the other fields
and confirms the resulting state by readback.

Captured motor, process and statistics frames are included as regression
vectors. This audit establishes captured field contents, not live hardware
verification of the new controls or the behavior of another pump model.
