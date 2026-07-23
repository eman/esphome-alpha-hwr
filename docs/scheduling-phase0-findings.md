# Scheduling — Phase 0 Bench Characterization (findings)

Bench-verified on the physical ALPHA HWR (2026-07-22) with a temporary
debug firmware that logged the raw `ClockProgramOverview` and weekly-interval
bytes plus `control_source`. This record anchors the scheduling work.

## ROOT CAUSE of the "inverted / pump-will-be-idle" schedule (PROVEN, then FIXED)

The long-standing report — the Grundfos app showing the coordinator's schedules
as **"pump will be idle"** while app-made schedules showed **"pump will run"** —
was **not** a run/idle inversion in the windows. It was the overview's
`default_action` byte, proven live on hardware (2026-07-22) with the user
watching the app:

- The app labels a schedule from **`default_action`**:
  `Stop (0x01)` → **"pump will run"**; `Auto (0x02)` → **"pump will be idle."**
- **The Grundfos app always writes `default_action = Stop`.** Our firmware
  **never set it — it preserved** whatever was on the pump. So if the pump's
  `default_action` was `Auto`, every coordinator/firmware schedule inherited it
  and the app rendered the whole thing as "pump will be idle."
- Bench proof: forcing our commit to write `default_action = Auto` made the app
  show "pump will be idle We 11:00 AM – 12:00 PM"; forcing `Stop` flipped the
  same schedule to "pump will run." One byte.

**Fix:** schedule writes now explicitly set `default_action = Stop` at all three
overview-write paths (`set_state`, `set_state_async`, `send_configuration_commit`),
matching the app. Interval `action` (Auto), the upload payload, and the canonical
hash are unchanged — the external scheduler and RFC-005 need no changes.

## RESULT: scheduling works, and Auto windows = RUN (directly observed)

Two clean, hands-off runs (no writes sent at the window edges) confirmed the
pump follows its schedule:
- **19:10 window** — at the start edge `op_mode` went `1(STOP)→0(AUTO)` and the
  motor spun to ~3670 RPM.
- **20:48–20:50 window** — motor `0→3508→~3670 RPM` at **20:48:02**, ran the whole
  window, and dropped to `0 RPM` at **20:50:02** — i.e. ran exactly for the
  scheduled window and nothing outside it.

So **there is no run/idle inversion**: Auto windows are exactly when the pump
runs (idle is the complement), matching the bytes and the vacation semantic
(Stop = "pump inactive" ⇒ Auto = run).

**The one precondition that gates everything:** the pump must be `op_mode=AUTO`
(enabled, **not** sitting in a manual STOP). A manual STOP (e.g. our
`pump_set_enabled=0`) blocks the schedule entirely; a setpoint write flips the
pump to Remote and also takes it off the schedule. Every earlier "the schedule
does nothing" observation traced to the pump being parked in a manual STOP —
not to any inversion. Once enabled/AUTO, the schedule drives it reliably.

## Authoritative on-wire schedule model (from raw bytes)

`SchedulingActionType`: `None=0, Stop=1, Auto=2, SetpointOffset=3, MixitEcoMode=4`.

**The pump's actual stored schedule** (26 enabled weekly intervals, all layers):
- **Every interval `action = 0x02` (Auto = RUN).**
- Overview `default_action = 0x01` (**Stop = idle**), `clock_program_enabled = 1`.
- Overview raw `[02 05 00 05 01 01 00 00 00 00]`: `max_nof_actions=2`,
  **`max_nof_single_events=5`** (this pump's real slot count, not 35),
  `max_nof_events_per_day=5`, `enabled=1`, `default_action=1(Stop)`, `base_setpoint=0`.

**Conclusion: windows are RUN periods; idle is the complement (default_action=Stop).**
There is **no run/idle inversion** at the encoding level — our firmware's
hardcoded interval `action=0x02` matches what the pump/app actually use. The GO
`pump_will_idle` caption describes the out-of-window default, not the windows.

## Firmware defects confirmed

1. `schedule_service.cpp:210` comment says `default_action = START` but `0x01 = Stop`.
   The written **value is correct**; the comment/label is wrong.
2. The firmware hardcodes every interval/event `action` to `0x02` and
   canonicalizes/hashes it away — so it cannot represent a `Stop` (idle) interval
   or a `Stop` vacation event, and cannot read back the pump's real action byte.
   (Functionally fine for the current all-Auto schedule, but blocks idle windows,
   Home-app-style active/inactive schedules, and vacation.)

## Control-source interaction — the key operational finding

`control_source`: `1 = Local/Panel`, `2 = Remote/Digital`. Our component's
setpoint/mode/enable writes put the pump in **Remote (2)**.

Bench observations:
- **Remote control ⇒ the schedule is ignored.** After our Constant-Speed write
  (control_source→2), the pump ran continuously straight through two clean
  window-end edges (14:20, 14:42) — the schedule did not stop it.
- **Master STOP overrides the schedule.** With `op_mode=STOP` (Pump Enabled off)
  under Local control, an Auto window start-edge did **not** start the pump.
- An earlier apparent "stop at a window end" (Temperature Control, 14:09) was
  **temp-autoadapt idling**, not the schedule — a confound. Constant Speed
  (which does not self-idle) disproved it.

This matches the two user observations: enabling the pump started it immediately,
and **the GO app forcibly disables the schedule when you press Start** — manual/
remote control and schedule governance are mutually exclusive.

## Control-source is per-write-type (bench-confirmed)

- `pump_set_enabled` (Class 3 START/STOP) and `pump_set_mode` (mode only) **keep
  the pump in Local control** (Remote Mode entity stays False / `control_source=1`).
- `pump_set_setpoint` (mode + value) **flips the pump to Remote** (`control_source=2`).

This matters: enabling/mode-setting alone does not take the pump off local/schedule
control, but writing a setpoint does.

## RESOLVED via BLE captures — manual control disables the schedule

Decoded from `resources/traffic_capture/` (tshark, GENI writes to handle 0x1a):

- **`schedule_on_off.log`** — enabling/disabling the schedule is a **standalone**
  ClockProgramOverview write (Class 10 OpSpec 0x93, Obj 84 `0x54`, sub 1,
  type 0xDA01, 10 bytes). Byte 4 = `clock_program_enabled`: `01` enables,
  `00` disables. No start/stop command present.
  `…0a9354000100da0100000a 02 05 00 05 [01|00] 01 00000000`.
- **`start_stop_pump.log`** — a manual **start/stop** is the control write
  (Class 10 Obj 86 `0x56`, sub 6): `…0006 01 2f01 0000 07 00 [00|01] 02 45657000`
  where the byte after `07 00` is `00`=START / `01`=STOP. The capture also
  contains overview writes setting `clock_program_enabled=00`. These are two
  distinct writes, **not** one fused command.

**Conclusion: manual control and the weekly schedule are mutually exclusive.**
Per the app's behavior (user-reported), the app will not let you start the pump
without the schedule disabled — you disable the schedule (a standalone overview
write) as a separate step, then start. Both are just writes; the constraint is
"schedule must be off to run manually," enforced at the app/UI level. The
invariant that matters: the schedule only governs the pump when it is enabled
**and** the pump is `op_mode=AUTO` (not parked in a manual STOP), as the
directly-observed 19:10 / 20:48 runs above confirm.

### Firmware / coordinator implications (the real fix)

- **Our `pump_set_enabled` does NOT disable the schedule** the way the app does.
  So we can leave the pump in a state the app never creates — schedule enabled
  *and* a manual start — with undefined precedence. `pump_set_enabled` (and any
  start/stop) should mirror the app and write `clock_program_enabled=0`, or the
  interface should otherwise make the manual-vs-schedule mode explicit.
- **For scheduled operation the coordinator must enable the schedule and issue
  NO manual control writes** (no start/stop, no setpoint). Any such write takes
  the pump off the schedule. This — not a run/idle inversion — is the mechanism
  by which "the schedule doesn't do what we intend."
- Returning to schedule control = re-enable the schedule (standalone overview
  write, byte4=1) and stop issuing manual commands.

## (superseded) Could NOT reproduce schedule run-state gating via the ESPHome interface

With an unambiguous run signal (Constant Speed, set mode-only so it stayed Local)
and the schedule enabled, the pump **ran continuously straight through both a
window end-edge (15:42) and start-edges** — the schedule did not stop or start it.
Every apparent "stop at an edge" occurred only in Temperature Control and is
attributable to **autoadapt idling** (no heat load on the bench), not the schedule.
Net: **through our component I could not get the pump to follow its schedule's
run/idle windows at all**, under Local or Remote control. Whether this means (a)
any component write suppresses schedule governance, (b) the engine re-evaluates
only at times my tests missed, or (c) governance requires pure app/panel control,
is **unresolved on the bench** — it likely needs observation under the GO/Home app
with no ESPHome writes. This is itself the most coordinator-relevant open question:
if the pump won't follow its schedule while driven by our interface, neither will
the coordinator through it.

## Implication for the coordinator (the likely real issue)

The uploaded schedule is encoded correctly (Auto/run windows, Stop default). But
**a coordinator that drives the pump through our component's remote writes puts
the pump in Remote control, where the schedule is ignored.** The clock program
only governs the motor under local/schedule control and when not master-STOPped.
So the failure mode is not "idle instead of run" at the byte level — it is that
remote control bypasses the schedule entirely. This must be confirmed and
handled (return the pump to schedule/local control after upload; or reconcile
how the coordinator drives run-state) before the schedule can do its job.

## Open items for the next pass

- Determine exactly how the pump (re)enters schedule-governed local control from
  a Remote or master-STOP state (physical panel? a specific write? schedule
  re-enable while not STOPped?). Needs a follow-up under local control without
  remote writes.
- The plan's "inversion" framing must be corrected to this control-source +
  action-model framing across firmware, RFC-005, coordinator, and docs.

## Bench-session caveat

During characterization, schedule writes touched **only layer 0 / Wednesday**;
it was restored to its real value 07:14–07:20. Read-back caching made the
initial baseline unreliable — the fresh post-reboot read (26 entries) is the
pump's true schedule. Please verify the weekly schedule is as intended; the
clean restore path is a coordinator re-upload against the canonical hash.
