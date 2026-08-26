# Changelog

## [Unreleased]

## [0.16.0] - 2026-08-25

### Migration

Three breaking changes land in this release. Two of them fail loudly; the third
does not, and is the one to read.

| Change | What breaks | What to do |
| --- | --- | --- |
| The six pump services are renamed | The automation fails with `Action ... not found` — **the next time it fires**, not when it is reloaded, so a rarely triggered one goes on looking healthy | Rename per the table below |
| `droplet_max_stale_seconds` → `flow_max_stale_seconds` | `esphome config` fails and names the replacement | Rename the key. Default (60 s), behavior and semantics are unchanged |
| The Head sensor is named "Head", not "Head Pressure" | **Nothing fails.** The renamed sensor arrives as a *new* entity and `sensor.<device>_head_pressure` is orphaned with its history and long-term statistics | To keep the history, declare `name: "Head Pressure"` under `head:` in your own `alpha_hwr:` block. Otherwise delete the orphan and clear its statistics under **Developer tools → Statistics** |

**The service renames, in full:**

| Old service | New service |
| --- | --- |
| `pump_set_enabled` | `set_pump_enabled` |
| `pump_set_mode` | `set_mode` |
| `pump_set_setpoint` | `set_setpoint` |
| `pump_set_temperature_range` | `set_temperature_range` |
| `pump_set_cycle_times` | `set_cycle_times` |
| `pump_set_state` | `set_pump_state` |

Read the table rather than transposing by hand: `pump` is *dropped* where the
verb alone is unambiguous (`pump_set_mode` → `set_mode`) and *kept* only where
it says which thing is being set (`pump_set_state` → `set_pump_state`, since the
schedule has a state too). Arguments, semantics and every field of
`esphome.alpha_hwr_write_settled` are untouched — the services move onto the
names their settle events were already reporting. The schedule services
(`set_schedule_entry`, `upload_schedule`, `clear_single_event` and the rest)
were already correct and do not move.

Finally, update the version pin in your own configuration alongside the examples,
which now resolve to `@v0.16.0`.

Each of these is written up in full below, with the reasoning and the failure
modes: the service renames and the two key/entity renames under **Changed**.

### Added

- **`connect_after_boot_delay`**, which holds the first BLE connection so a log
  client can attach before the link opens (issue #310, reported by
  @jfriend00). On a stock node the whole BLE phase — connect, bond, encryption
  request, service discovery, notification enable — is over before an
  `esphome logs` stream carries a line: `setup()` runs at
  `setup_priority::DATA`, long before the API server is up. The reporter's
  capture opens on the banner and finds the session `STABILIZING -> READY`
  0.59 s later, with none of the sequence in it. That phase is where connection
  problems live, so it is the one worth capturing; pair it with
  `frame_logging: true` and one capture covers the BLE phase and every GENI
  frame after it.

  `0s` by default, and that is what a deployed node should keep — it delays the
  pump becoming controllable by exactly the interval set, on every boot.

  **Pump Link Status gains `Boot delay`** for the duration, rather than showing
  `Initializing`. The option is used precisely when somebody is watching that
  sensor, and `Initializing` is indistinguishable from a pump that cannot
  connect — the confusion the status ladder exists to remove. The value is
  unreachable unless the option is set.

  Three places treat a deliberately absent link as a broken one, and all three
  are handled: the boot grace now covers the hold (without it a hold near 15 s
  publishes `Unreachable` for a link held down on purpose); a suspend arriving
  inside the hold is not undone when it elapses; and releasing that suspend
  inside the hold does not connect early. The other three mechanisms that watch
  the link — the inbound-data watchdog, the readiness watchdog and the gap
  histogram — are all armed from the first connection *open*, so a hold that
  ends before any open cannot reach them.

- **`frame_logging`**, a component option that logs every GENI frame, sent and
  received, whole, at INFO. Previously the receive path logged only the first 12
  bytes and only at VERBOSE, and the send path logged nothing.

  Off by default. With it off the existing VERBOSE receive line is unchanged and
  nothing additional is allocated. Frames are logged at INFO rather than the
  `ESP_LOGV` that AGENTS.md §3 assigns to packet dumps, so a capture reaches every
  API subscriber; `docs/configuration.md` carries the subscriber and
  `tx_buffer_size` cautions.

- **The flow limiter can be set, not just read** (issue #299, `set_flow_limiter`).
  For three of the pump's five modes the limiter **is** the flow control:
  constant curve and constant pressure regulate speed and head, so flow drifts
  with hydraulics and the cap is the only thing bounding it, and per manual
  §9.3.2 temperature control deliberately runs up its maximum curve *until it
  reaches the flow limit* — making the limit the operating point rather than a
  safety net. It was readable and unreachable.

  The vendor's own reasons for capping flow are ordinary owner concerns rather
  than commissioning-only ones: §10.4 gives system noise, §9.3.2 and §9.3.3 give
  flow-accelerated corrosion.

  **It is a read-modify-write, and has to be.** Type 895 is one struct —
  `[name][enable][limit][kp][ti][td]` — so setting the cap rewrites all eighteen
  bytes, and the three PID floats are echoed verbatim from a mandatory pre-write
  read. A write that zeroed them would re-tune the pump's limiter control loop
  invisibly. If the record cannot be read the write is refused rather than sent
  blind, and the decode now keeps the PID bytes precisely so there is something
  to echo.

  The cap settles against the pump's factory bounds (86/620–621): outside them
  it settles `clamped` with the stored value reported, and a pump that keeps its
  old cap settles `rejected` — the same distinction the setpoint write draws,
  and two answers a client acts on differently.

  **It does nothing in Cycle Time mode**, per the scope table from #274. The
  write succeeds, the pump stores it, and the cap is ignored while that mode
  runs. Documented rather than blocked: the record really is stored.

  **Four entities alongside the service**, raised by @jfriend00 in review: every
  other numeric value the pump stores as a tunable setting is a `number` entity,
  and this was the one exception, reachable only through a service call. `Max
  Flow Limit` / `Min Flow Limit` (`gal/min`, no `device_class`, so Home
  Assistant does no unit conversion and the entity shows exactly what the
  service takes) and `Max`/`Min Flow Limit Enabled` switches. The cap and the
  enable flag are separate controls over one record — the same shape
  `set_pump_state` and the `Engage Pump` / `Schedule Enabled` pair already
  use — with each keeping the other's stored value so they cannot fight.

  All four are `optimistic: false` and read through a new component-level
  `limiter_state()` getter. That getter is what makes it possible: without it
  the only published limiter state is the text sensor's prose, and a config
  parsing `1.60` back out of "MaxFlow enabled at 1.60 gpm (not limiting)" is
  precisely the brittleness this cluster has been about. Verified on the pump:
  changing the cap through the service moved both the number and the switch.

  On the hazard this was deferred over — that enabling a limiter *silently* caps
  the pump. That was a property of the old blindness rather than of the write,
  but only where the read is switched on, and `flow_limiter` / `flow_limited`
  are optional and off by default. The settle event carries the confirmed record
  either way, so a write is never fully silent; what is missing without the
  entities is the ongoing "is it limiting". The component now says so at WARN
  once per write rather than refusing one because a diagnostic entity is
  undeclared.

- **The repository is MIT licensed** (`LICENSE`). It had no licence at all,
  which the HACS validator surfaced while working on issue #183 — and it
  mattered well beyond HACS. A public repository with no licence is *all rights
  reserved* by default, so strictly nobody had permission to use, copy or modify
  what the README invites them to install. The text is byte-for-byte canonical
  MIT, which is what GitHub's licence detection matches on.

- **The Class 7 string decode is a pure function, so its memory-safety guard is
  provable again** (issue #282). `read_class7_string_async()`'s callback computes
  `string_len = len - HEADER_LEN - CRC_LEN` in `size_t` arithmetic; a frame under
  8 bytes wraps that to ~1.8e19 and the copy loop reads ~127 bytes past the
  frame. `MIN_FRAME_LEN` is what stops it.

  The decode lived in a lambda inside a private method, so the only way to reach
  it was through a `Transport` — and once #278 gave `on_notification()` a length
  floor of its own the two guards masked each other's mutations: relaxing the
  inner one is invisible because the outer one rejects the frame first, and
  removing the outer one is invisible because the inner one still refuses to
  parse it. CI found it as an equivalent mutant, 266/267 on #279.

  `protocol::decode_class7_string()` is now pure and callable with arbitrary
  bytes, for the reason `schedule_codec`, `telemetry_decoder` and
  `response_match.h` are — Class 7 was the odd one out. A host test hands it a
  7-byte frame with no transport in front of it, and the `len < 5` relaxation
  fails three assertions again. Both mutation directions are back, and the
  upward one is kept alongside because it stays provable if the transport's
  floor moves again.

  **Both guards stay.** The transport's floor is about framing and this one is
  about memory safety, and a unit that parses bytes should not have to assume
  anything about who handed them over. Two protections masking each other is a
  documentation problem, not a duplication problem.

  The null-buffer case is now reported distinctly from the too-short case, the
  overlong case is truncated *and says so* rather than silently, and the count
  byte remains reported-but-never-trusted to bound the copy — trusting a
  radio-supplied count is an overread waiting to happen from the other direction.

- **A bad-CRC frame drop is counted, not just logged** (`link_crc_drops`, issue
  #260). A frame that fails its CRC was dropped correctly and then left no trace
  anyone could find: no counter, no entity, nothing in Home Assistant, the whole
  record being one `ESP_LOGW`. So a link quietly shedding frames was
  indistinguishable, from outside, from a component that occasionally timed out
  for no reason.

  That check is doing load-bearing work — before it, every control, schedule,
  single-event, event-log and device-info payload, *including the readbacks that
  decide write verdicts*, was parsed from unverified bytes. How often it fires is
  a property of the link worth knowing, and it could not be collected.

  The counter follows the existing link-diagnostic conventions: off by default,
  `total_increasing` so a run survives the reboots it will meet, and not cleared
  by a disconnect — a dropped link says nothing about how many frames the radio
  corrupted. `tools/link_gap_report.py` picks it up alongside the gap histogram
  at no extra collection cost and prints it **per watched hour**, because a raw
  count answers nothing: the one occurrence on record was "one in roughly 5,900
  log lines", which has no denominator. Where the run saw zero, the report says
  what bound that supports rather than declaring the link healthy.

  The warning itself now carries the received length, the declared length and
  the leading bytes:

  ```
  Dropping frame with a bad CRC (received=59, declared=56, head=24 34 F8 E7)
  ```

  A bit-flip in the payload preserves the frame length, so `received ==
  declared` with a bad CRC points at the radio, while `received > declared`
  means the notification carried bytes past the end of this frame — a
  misassembly rather than noise. One line, and it turns the next sighting into
  evidence instead of another anecdote.

- **The pump's own daylight-saving rule is read and checked against the node's
  timezone** (`Pump Clock DST`, issue #286). The pump keeps a DST rule of its own
  and applies it to its own clock, twice a year, independently of anything the
  node believes. `DaylightSavingTime` (Object 94 SubID 102, type 323 v1), read
  from the bench unit: enabled, second Sunday of March to first Sunday of
  November at 02:00, 60-minute offset — the US rule.

  Schedule windows are stored in the pump's **local** time, and
  `utc_to_local_unix()` takes that offset from the **host's** zone. The
  conversion preserves the user's wall clock across a transition — an 07:00
  event stays at 07:00 — but only while the two rules agree. They can differ
  without anyone doing anything strange: a pump shipped for one market and
  installed in another (the rule is stored per unit and is writable), a node
  whose `time_id` zone is not the pump's locale, or a pump with DST disabled on a
  node whose zone observes it. That last one moves every stored event by an hour
  relative to the pump's clock, twice a year, silently.

  The failure is the same shape as #263 — an event stored at an instant nobody
  asked for, confirming clean, because the confirm applies the same wrong offset
  in reverse. #263 fixed the case where the arithmetic *wraps*; this is the case
  where it is against the wrong rule, and an hour is comfortably inside every
  bound the component has.

  The new `Pump Clock DST` text sensor reads `OK` with the rule spelled out when
  they agree, and names **both** rules when they do not — "my schedule moved an
  hour in November" shows up months after the cause, so a bare "mismatch" would
  leave the user no closer to it. The node's own rule is derived by observation
  (sample the year, bisect for the transition) rather than by parsing a TZ
  string, because there is no portable way to ask libc for its rule.

  **Reported, never corrected.** The Grundfos GO app, this component and the
  Python library all write this pump's clock, and the pump cannot say which base
  a value arrived in — a component that silently rewrote the pump's rule would be
  the third party in a fight the user cannot see. Whether the component should
  ever write 94/102 is left open deliberately.

  The read is only issued when the entity is configured, so a node that does not
  display it does not spend a round trip per connection on it.

  **Rebuilt after review before shipping.** The first implementation was
  withdrawn from #287 with two claimed defect classes, of which **one was real**:
  it named the wrong day whenever a transition fell at local midnight — 212 start-side wrong answers across the timezone database,
  affecting Havana, Cairo, Beirut, the Azores, Santiago and Nuuk. The probe now
  derives the date and the hour from one reading, advanced together.

  The second claimed defect — that reading libc made it inert on the device —
  was **wrong**: ESPHome overrides `localtime_r()` there, so the old probe
  worked. It samples through `ESPTime` now regardless, for the reasons in #289's
  correction, but that part was tidiness rather than a fix. It also refuses to emit half a rule for a year with a
  single transition (a country adopting or abolishing DST), and keys transition
  ordering on the offset *direction* rather than an `is_dst` flag, so a zone
  redefinition that flips the flag without moving the clock is not read as a
  shift. Verified against Python's `zoneinfo` for the six previously-wrong zones
  plus two controls.
- **The pump's flow limiters are read and surfaced** (`Pump Flow Limiter` and
  `Pump Flow Limited`, issue #274). The pump has MaxFlow and MinFlow limiters,
  set from the Grundfos GO app and entirely separate from the setpoint. This
  component read none of it.

  That is the worst shape a diagnostic hole comes in, because **every signal the
  component publishes says the write worked — and it did.** Measured by
  @jfriend00 on a real installation with MaxFlow at 1.6 gpm, constant speed:
  1700 RPM commanded delivered 1701, and 3000 RPM commanded delivered 1883, with
  flow pinned at 1.59 gpm. Every write settled `accepted`, and the pump reported
  the commanded setpoint back from its own 86/7 each time. At 3000 RPM it
  delivered 63% of what was asked, and nothing said so.

  `Pump Flow Limiter` distinguishes four states, and the distinctions are the
  point: `No limiter enabled`, `MaxFlow enabled at 1.60 gpm (not limiting)` —
  switched on but not yet biting, which it will the moment the setpoint rises —
  `MaxFlow limiting at 1.60 gpm`, and `unknown`, because a pump that has not
  answered is not a pump with no limiter. `Pump Flow Limited` is the same thing
  as one bit, for automations.

  Caps are reported in gallons per minute, the unit they were entered in: the
  values land on the wire in m³/s and every limit value seen on two pumps
  converts to an exact gpm figure.

  The configuration is read once per connection; the status is re-read on the
  control poll, because whether a limiter is *limiting* changes with the load
  while whether one is *enabled* changes only when somebody edits it in the app.
  One chain runs at a time — the records within each family share a type code
  and replies carry no request identifier, so two overlapping chains would feed
  each other's requests — and a chain stops at its first failure for the same
  reason.
  The whole family is dropped on a disconnect — it describes the pump we were
  talking to, and a limiter changed in the app while the link was down would
  otherwise be reported wrongly for as long as the node stayed up.

  Six addresses are read — 86/600, 601 (config), 640, 641 (status) and 660 (the
  manager, which names *which* limiter is binding) — and **not a sweep**. The
  profile declares twenty slots per family; all fifty-four others answer a
  nine-byte `OPERATION_FAILED` frame that is below the receiver's `len >= 11`
  gate, so it never matches and each one costs a full read timeout. Two
  independent client implementations hit that.

  Reading only. Enabling a limiter silently caps the pump, which is not a change
  to make as a side effect. The reads are issued only when one of the entities is
  configured.

- **`Cycle Flow` is documented, including that the vendor does not offer it**
  (issue #280). The control sets the flow the pump targets during the ON periods
  of Cycle Time Control. It regulates — bench-measured within 1% across four
  setpoints, with motor speed moving to hold it — and the Grundfos GO app has no
  equivalent, while the manual says the mode has no flow parameter at all.

  It stays, deliberately: the register split is what the pump's own layout says
  (Object 91 Sub 421's first field is a flow setpoint; Sub 430 has no flow
  field), and being undocumented by the vendor is not a reason to remove
  something that works. It is also **not** the MaxFlow limiter under another
  name — 2.0 and 3.0 gpm cycle-flow runs were delivered in full with MaxFlow
  enabled at 1.4 and 1.6 gpm. The discrepancy is now recorded in
  `docs/configuration.md` and beside the code, so the next person to notice it
  finds the answer rather than repeating the investigation.

- **A watchdog for links that are connected, streaming, and never usable**
  (`ready_timeout`, issue #211). Reported from a live installation: connected,
  pairing on, telemetry updating in Home Assistant, `Pump Ready` off
  indefinitely, no fault raised and no reconnect — with an automation gated on
  `Pump Ready` waiting silently forever while the dashboard showed a healthy
  pump. In the reporter's words, the one failure shape where the diagnostics
  actively point away from the problem.

  `data_timeout` could not catch it, and could not be tuned to. Its observable
  is silence and it is re-armed by every inbound notification; this pump
  volunteers telemetry unprompted, so a session stuck anywhere keeps re-arming
  it and it never fires. Liveness was watched and progress was not.

  The new watchdog is timed from connection-open and cleared **only** by the
  pump reaching its usable state. Nothing else resets it — not data, not a
  session transition, not a cache filling partway — and that restraint is the
  design rather than an omission: a timer refreshed by anything on the way to
  readiness chases the state it is waiting for and can never expire. That trap
  is not hypothetical here; `link_watchdog.h` already documents the link-status
  ladder refreshing its own timestamp and keeping a rung permanently
  unreachable, and the reporter predicted this instance of it before any code
  existed.

  On expiry the link is dropped, the usual reconnect takes over, and **Pump Link
  Fault** reads `Pump never became ready (300s)`. That string is the point of
  the change as much as the recycle is: from outside the component, *starting
  up* and *stuck* look identical, so naming the condition is the one thing an
  automation cannot do for itself.

  It backs off exactly as `data_timeout` does, sharing that code rather than
  copying it, and takes its own rank on the fault surface — below `DATA`, so a
  link that has genuinely gone silent keeps the more specific diagnosis.

  Fixing this also required fixing where the answer is *shown*. The fault sensor
  was blanked whenever the **session** was ready, and the session reaches ready
  two seconds after subscribe with no frame exchanged — which is on the far side
  of the reported failure. Latching this diagnosis and gating it that way would
  have published `None` over the one string that explained what was wrong. The
  gate is now the pump being ready, which is what "healthy" has always meant
  here.

  **It is two options, because naming a fault and reconnecting carry very
  different costs.** `ready_timeout` (default `300s`) only says what is wrong:
  it logs and latches `Pump never became ready (300s)`, escalating the window on
  each report. That is free, and it is the half the reporter said he could not
  build — an automation can see `Pump Ready` has been off a while but cannot
  tell *starting up* from *stuck*. `ready_recycle` (default `false`) is what
  tears the link down, opt-in because every forced reconnect takes another run
  at the window where an encryption failure can erase the pump's bond, which
  then needs physical access to restore.

  An earlier draft bundled them and shipped the pair off, which meant nobody got
  the diagnosis either.

  **The recycle half defaults off, and that is a deliberate retreat.** An earlier
  draft shipped it on at `300s`, reasoning that a node with no bond never gets a
  working link anyway, so the hazard was moot. That reasoning was not tested:
  the run meant to test it was confounded three ways — a pre-release build, with
  pairing enabled rather than at its default, at a signal level of −96 to −101
  dBm — and it bonded within 252 ms regardless (issue #245). So the
  configuration the default would be reasoning about remains unobserved, which
  is issue #244. Shipping a watchdog on, whose failure mode is a forced
  reconnect and whose safety rests on an unobserved case, is not a trade worth
  making; it is opt-in, for nodes that reliably reach ready today.

  The suggested value when enabling is `300s`, and it is
  bracketed on hardware by setting the window short and watching which value
  fired: 10 s fired, 20 s fired, 40 s did not. A fresh connection on a bonded
  pump therefore reaches usable in roughly 22 s, so the suggested value is about
  twelve times the measured figure. That bracket is one pump and a *bonded* reconnect;
  a first pairing is still untimed. Too loose still turns "silent forever" into
  "recovers eventually"; too tight recycles a pump that was merely slow. `0s`
  disables it.

  The same bench run verified the whole path end to end on hardware — fire,
  latch, count, back off, recover, clear — which no host test can: the fault
  reached the Home Assistant entity as `Pump never became ready (10s)`, `Pump
  Link Recycles` incremented, the window doubled, the next connection came up
  inside the widened window, and reaching ready cleared the fault to `None` and
  reset the counter to 0.


- **The re-pairing procedure, written down** (follow-up to #230). The recovery
  #238 points users at was stated as "put the pump into Bluetooth pairing mode",
  as though it were one button press. It is not, and the missing steps are not
  guessable while standing at the pump: the front panel auto-locks and is
  unlocked from the Grundfos GO app, and the button commonly needs several
  presses before it takes.

  The log line and README both now say the remedy is more than a button press
  rather than implying it is not.

  A first version of this entry claimed the node had to be stopped before the GO
  app could connect, on the grounds that the pump holds one BLE connection at a
  time. That is true of a node that is *bonded and connected*, and false of the
  case this is about: an unbonded node failing to connect has been observed, over
  about a dozen attempts, not to get in the app's way. The correction is in the
  page; the general constraint is recorded where it applies, along with the fact
  that there is no way to idle the link short of powering the node down.

  Reported by @jfriend00 on #229, from their own re-pairing routine. Recorded as
  one owner's procedure on one pump rather than as something this project has
  verified, and distinguished from clearing a bond *at* the pump, which is a
  different operation nobody here has needed. The same one-connection fact is
  now named in `docs/configuration.md` as the likeliest third cause of a pairing
  stall, where that paragraph previously hedged without saying what it meant.


- **A pump that will not pair is reported instead of waited on** (issue #230).
  Clearing only this node's bond — `ble_client.remove_bond`, an NVS erase, a
  re-flash that loses NVS — leaves the pump holding a bond for a peer that no
  longer has one. It then sees an unencrypted stranger at a bonded address,
  sends no security request, and drops the link. The node reconnected roughly
  every five seconds and logged "waiting for pump to initiate pairing" each
  time, indefinitely, while **Pump Link Fault** read `Failed To Establish
  (0x3e)` — a radio diagnostic for a link whose radio is fine.

  Nothing about the connect path changes, because the silent wait is correct:
  initiating from this side against an unbonded pump returns 0x52 ("Pairing Not
  Supported") and loses the pump's own request with it. That was tried on the
  bench and it is why the wait exists. What changes is that the wait is now
  bounded by a diagnosis rather than by nothing. After three consecutive
  connections that open with no bond, exchange no security and carry no data,
  the node logs a `WARN` naming both states it could be in and the one remedy
  that fixes either — put the pump into Bluetooth pairing mode, at the pump —
  and latches `Pump not accepting pairing` on the fault sensor. The state is not
  recoverable over the air, so saying so is the whole fix.

  It deliberately does not claim to know *which* state it is in. The evidence is
  an absence, so a pump bonded to a departed client and a pump that has simply
  never been put into pairing mode look identical from here; the remedy is the
  same in both, and asserting the first as fact would be an inference the
  evidence does not carry.

  Most of the design is about not crying wolf, because telling someone their
  pump refuses to pair when it does not is worse than the silence this replaces.
  `enable_pairing` defaults to `false` and passive telemetry needs no bond, so a
  healthy installation runs unbonded forever and its links open unbonded and are
  eventually dropped — exactly like a stalled one, except for what happened in
  between. Four things clear the count: a notification, a security request
  however answered, a completed bond, and a connection that opened bonded. Three
  kinds of ending are not counted at all — a failed open, a teardown this node
  initiated, and a link the radio lost, which is read from the disconnect reason
  rather than assumed. The last two matter most: without them the data
  watchdog's recycles and any run of supervision timeouts both read as refusals,
  and both would have replaced a true diagnosis with a false one.

  On the fault surface it takes its own rank, below both the subscribe fault and
  a real pairing failure, on the rule that **an observed event outranks an
  inference from an absence**. That is not where it started. The first version
  latched at the same rank as an SMP failure, and an adversarial review drove
  the case that breaks: issue #14's `0x61` erases the bond, every connection
  after it is unbonded and unanswered — so that failure manufactures the stall's
  own precondition — and the stall then replaced the root cause about fifteen
  seconds later. `0x61` is the only pointer to the `reconnect_settle_time`
  mitigation, and it is the one that recurs. The stall also withdraws itself the
  moment it is refuted, rather than waiting to be overwritten: on a default node
  nothing else would ever take it off, since `AUTH_CMPL` never fires and a
  stalled link never reaches ready.

  The log repeat is throttled to about once a minute while the fault string
  holds continuously — the two have different jobs. The rule is in
  `pairing_stall.h` with its own host test, so it can be exercised
  exhaustively; the wiring is tested in `test_component_wiring.cpp`, which was
  already compiling `ble_connection_manager.cpp` despite three headers still
  claiming nothing does.


- **A gap histogram, so the `data_timeout` default can be chosen from
  observation** (issue #176 part 1). Eight optional diagnostic sensors:
  `link_gaps_over_15s` through `link_gaps_over_90s`, `link_gaps_truncated`, and
  `link_watch_time`.

  `link_max_gap` reports the worst quiet interval since boot. That is one
  extreme value, and the question the default actually turns on is a rate — a
  budget of `T` fires once for every quiet interval longer than `T`, so what it
  costs is how many of those happen per day. Each `link_gaps_over_Ns` counter is
  exactly that number, because the comparison is the same strict one the
  watchdog makes.

  The rungs are sized against the timings already recorded in
  `link_watchdog.h`: 15s and 20s are one missed poll cycle, 30s and 45s sit
  clear above the worst case for reaching first data after a connect (they
  straddled it at 21.5s and 31.5s when they were chosen; removing the opening
  sequence took that to 16.0s, and the rungs stay put so the counts already
  gathered against them remain comparable), 60s is the shipped default, and 90s sits above it deliberately — without a rung
  there the data cannot separate a 70s excursion, which a longer default would
  cover, from one lasting minutes, which is a genuine link death that should
  recycle. Those two readings argue in opposite directions.

  All of it is fed from the sampler's existing `sample_()`, so the counters see
  exactly the intervals the maximum sees and inherit every censoring decision
  already argued and pinned there. `total_increasing` rather than
  `measurement`, which is the point of numeric counters over a text summary:
  they are RAM values that restart at every boot, and Home Assistant's long-term
  statistics recognise the reset and keep accumulating, so a run measured in
  weeks survives the OTAs it will certainly meet.

  Two additions the review insisted on, both because the failure they prevent
  looks like success:

  - **`link_gaps_truncated`.** An interval cut short by a recycle or a drop is a
    lower bound, so a run full of them has a tail that was cut off rather than
    observed — and without a count of them that reading is indistinguishable
    from a clean one. This statistic already made exactly that mistake once: a
    maximum reading 2.6s against a budget it had breached five times.
  - **A warning when `data_timeout` is too small for the rungs declared.**
    Under the `60s` default nothing can ever be recorded above 60s, so a run
    left at the default produces reassuring zeros whatever the pump does. A
    measurement run wants `data_timeout: 600s`. Emitted at *config* time, from
    `esphome config` and `esphome compile`, because the equivalent boot-time
    warning runs before the API server is up and so reaches the serial console
    only — confirmed on the bench, where it is absent from the log stream of a
    boot that emitted it. The boot warning is kept for serial users.

  The entities are **off by default**: they are an instrument for one decision,
  and switching them on costs eight diagnostic entities plus a boot warning on
  every install to answer a question only the people running the measurement are
  asking. `packages/alpha_hwr_pairing.yaml` carries the block commented out with
  instructions; `tests/ci-compile.yaml` declares it so the schema and codegen
  stay covered in CI.

  `link_watch_time` is the denominator — the time the counts were drawn from,
  obtained as the sum of the sampled intervals rather than from a second clock.
  It is the one value here that moves on every notification, so it is throttled
  to one publish per 300s on top of the change gate; that is Home Assistant's
  short-term statistics bucket, so the throttle costs no resolution anything
  downstream can use, and without it this would be a frame per API subscriber
  every 10 seconds forever (issue #127).

- **`tools/link_gap_report.py`** — reads those counters off one or more nodes,
  pools them across boots the way `total_increasing` does, and prints what each
  candidate default would have cost in recycles per day, with the decision rule
  printed alongside the answer. It refuses to recommend anything when the
  evidence does not support it: under two weeks of watched link, a material
  fraction of intervals truncated, or a run whose budget could not let the rungs
  fill. Any number of nodes pool; a recommendation describes the installations
  that reported it, which is a caveat rather than a gate.

- **The component's BLE lifecycle is host-tested** — `tests/test_component_wiring.cpp`,
  16 assertions, plus the ESP-IDF and ESPHome mocks that make it possible
  (issue #174 audit tail). `alpha_hwr.cpp` and `ble_connection_manager.cpp` are
  the two largest files in the repo, 2,090 lines between them, and
  `esphome compile` was the only thing that ever built either. They own every
  GATT and GAP event the pump can produce.

  The tests drive the real entry points — `gattc_event_handler()`,
  `gap_event_handler()`, `setup()`, `loop()` — and observe through `Pump Ready`,
  which is what a user sees, rather than through private state. Covered: the
  scan filter's accept and reject cases, that a failed GATT open starts nothing,
  that readiness is not a timer, that a disconnect reports not-ready, and that
  GAP events are filtered by address so a stranger's pairing failure cannot
  disconnect the pump (the #201 defect, now asserted end to end rather than only
  at the pure predicate).

  **The full connection now runs end to end in a host test**: a GATT link
  brought up event by event, the opening sequence's ten packets answered frame
  by frame with CRC-valid replies through the real transport, and the initial
  read chain driven until `Pump Ready` turns on — then off again on
  disconnect. Nothing is called on the component's behalf. Three mutation
  entries pin it, including one that stops the handshake reporting completion,
  which takes the whole chain down with it. The mocks are deliberately thin: where the real
  stack would do something asynchronous the mock records the call and does
  nothing, because a mock that invents the asynchronous half can hide the bug it
  was written to catch.

- **`link_recycles` and `link_max_gap`** — two optional diagnostic sensors for
  the inbound-data watchdog (issue #176). `link_recycles` counts consecutive
  recycles that produced no data and resets on a notification received once the
  session is ready (a deaf pump still answers the handshake, so those frames
  cannot count as proof the link works), so it reads 0 in normal operation and
  an automation can threshold on it; the Pump
  Link Fault sensor shows a reason only *during* a reconnect and clears on
  recovery, which makes repeated recycles a flap cadence somebody has to be
  watching to notice rather than a value. `link_max_gap` records the longest
  quiet interval since boot — every interval the watchdog times, however it
  ended — which is what a `data_timeout` default chosen from observation rather
  than from a constants calculation has to be based on.

- **The Lovelace card has host tests** — `tests/js/test_schedule_card.js`, run
  by `make test-js` and in CI. 1,800 lines that had shipped broken twice for
  want of one (every service call omitted the required `op_id`; the
  single-event regex could not match the format the firmware emits) now have 46
  tests over the write path, the event parser, the wire format of every
  payload, the rendered button state, the escaping of device-supplied strings,
  and subscription lifetime. Plain node against a stubbed DOM: no npm, no
  lockfile, nothing to install or keep current.

  The first version of this suite was itself unsound, and an adversarial pass
  is what found it. The harness left the Quick Run panel closed and the entity
  states empty, so the whole single-event UI rendered as `''` in every test —
  the Save button's `data-action` could be deleted with the suite still green.
  Worse, the XSS test used a payload with one of each metacharacter, so it
  passed against a non-global `replace` that is a live injection. Both are
  fixed: the harness renders the real card, and the payload repeats every
  metacharacter and asserts the exact escaped string.

- **`hwr-pump-dhw-example.yaml`** — the paired pump + control UI + `dhw_demand`
  combination, validated in CI alongside the other examples. It was the one
  documented recipe with no file behind it, which is why it broke unnoticed:
  `alpha_hwr_controls.yaml` reads `id(motor_speed)` directly, so this is the
  only configuration where renaming the rpm sensor fails.


- **Host tests for the session FSM** — `tests/test_session.cpp`, 36 assertions.
  The state machine had been documented in prose and in a diagram and checked by
  nothing, which is how a state with no way into it survived. Every transition
  that exists is now pinned, from every state, along with two behaviours a
  reader would not guess: `on_authenticating()` is reachable from READY for
  re-authentication and is idempotent, and the out-of-order guards **warn and
  then transition anyway** rather than refusing — so anything able to call
  `on_authenticated()` after a disconnect would drive the session to READY. That
  safety lives in the callers, not in the FSM, and the test now says so.

### Changed

- **Two outbound APDU hex dumps moved to `ESP_LOGV`** (issue #307).
  `write_temp_range APDU:` was logged at **INFO, unconditionally**, so it fired
  on every temperature-range write on a default build — the packages ship
  `logger: level: INFO` — costing an API frame per connected subscriber plus the
  `std::string` the formatting allocates. That is exactly the per-subscriber
  cost `docs/configuration.md` warns about under *Log level and API
  subscribers*, and the load that has exhausted this node's heap in ESPHome's
  outgoing buffer (issue #127).

  `Clock SET APDU:` was at DEBUG. Cheaper — a clock write happens twice a day,
  not per user action — but there was no reason for two identical lines to sit
  at two different levels, and AGENTS.md §3 assigns packet dumps to `ESP_LOGV`
  either way (`ESP_LOGD` is for single packet *summaries*).

  Neither line is deleted, but neither is the way to see these writes any more:
  `frame_logging: true` dumps the whole telegram each APDU is carried in, in
  both directions, without turning the rest of the component up to VERBOSE.

  **What actually changes, per level**, since the two lines started at different
  ones. The shipped packages set `logger: level: INFO`; ESPHome's own default,
  which a config not using the packages gets, is `DEBUG`.

  | Your `logger` level | Before | After |
  | --- | --- | --- |
  | `INFO` (the packages' setting) | temperature-range dump only | neither |
  | `DEBUG` (ESPHome's default) | both | neither |
  | `VERBOSE` | both | both |

  So on a packaged node one line disappears, not two — the clock dump was
  already below the shipped level and was never in that output. The clock change
  bites at `DEBUG`, where the line was visible and now needs `VERBOSE`.


- **The Lovelace card installs through HACS, and moved to `dist/`** (issue
  #183). The card is a Home Assistant frontend resource, so ESPHome cannot
  install it — and the only documented path was copying it into `/config/www`
  by hand. That meant every installation ran whatever version the user last
  remembered to copy, and the card has drifted out of step with the firmware
  **twice, both times silently**: a required service argument it never picked up
  made every write a no-op, and a display change left the Quick Run list empty
  in a way indistinguishable from "no events exist".

  Add this repo as a HACS **Dashboard** custom repository and HACS installs the
  card, registers the dashboard resource itself, pins it to a release tag and
  raises update notifications. The manual copy stays documented for installs not
  running HACS.

  **Two corrections to the plan the issue sketched**, both from checking the
  HACS documentation rather than building on the assumption:

  - HACS resolves a plugin file from **`dist/` first**, then the latest release,
    then the repo root — not "release assets, then `dist/`, then root". A `dist/`
    file therefore shadows a release asset entirely, so attaching the card to the
    release would not have done what was intended.
  - Version pinning does not need release assets anyway. HACS resolves the
    version from the release tag and installs the tree at that tag, so a card in
    `dist/` is pinned for free.

  That settles the issue's open question — move the card or publish a copy — in
  favour of moving it. A published copy alongside a source of truth is two paths
  that can diverge, which is the same shape as the drift this change exists to
  prevent.

  CI now runs **HACS's own validator** (`hacs/action`, category `plugin`), so
  "does this still install through HACS" is a checked fact rather than a claim.
  It covers what a hand audit cannot: `hacs.json`'s schema, the repository
  metadata HACS requires, and that the plugin file is where HACS looks.

  It found two gaps on its first run. **The repository had no LICENSE file** —
  fixed, see Added above. The other, that the README has no images, is a
  default-store criterion rather than a custom-repository one and does not
  affect installing today.

  Both are ignored in the job, and the licence one carries a note to remove it
  after this merges: that check reads GitHub's repository-level licence field,
  which GitHub derives from the **default branch**, so a `LICENSE` that exists
  only on a feature branch cannot satisfy it. Adding the file and removing the
  ignore in the same PR cannot both pass — worth knowing before someone tries.

  Everything else gates, including the `hacs.json` schema check — the failure a
  hand audit misses and a user discovers.

  `hacs.json` carries only `name` and `filename`. An earlier draft added
  `render_readme` and a `homeassistant` minimum version; both were dropped as
  unverified — `render_readme` is absent from the current key list and has open
  bugs, and a minimum-version floor nobody has tested is a guess that can lock
  users out.

  The card also carried a local `v6` marker unrelated to any release, so nobody
  could tell which firmware a given card matched. It now carries the release
  version, stamped by `tools/bump_version.sh` (which covered the example YAMLs
  and packages but not the card), and logs it to the browser console once on
  load — the only place a hand-copied card can be identified.

- **Where the flow limiter actually binds is documented** (issue #274). An
  enabled limiter is *not* a general explanation for a setpoint that is not
  reached, and treating it as one would be wrong in the more misleading
  direction. Bench-established across five modes: it binds in constant curve and
  constant pressure (flow follows the cap, not the setpoint — a commanded
  3000 RPM delivered 1885 RPM and 1.59 gpm against a 1.6 cap), is presumed to
  bind in temperature control, and is **ignored outright in cycle time**, where a
  2.0 gpm setpoint was delivered in full against a MaxFlow of 1.4.

  The app's display turns out to be an accurate map of that scope, and the manual
  agrees — §9.3.1–9.3.3 mention flow limits, §9.3.4 and §9.3.7 do not.

  None of it is encoded, and the reason is now written down: the entities read
  the pump's **status** registers (86/640, 641, 660), never the enable flag, so
  they report "not limiting" in cycle time because the pump is not limiting —
  with no mode table to keep in step with firmware. A finding that cost an
  afternoon of bench work was living only in an issue comment.

- **Cycle Flow is documented as a supported field the vendor hides, not a
  happens-to-work capability** (issue #280). The doubt was reasonable: the
  Grundfos GO app shows no such control on any ordinary screen, and the manual
  (§9.3.4) says the mode runs on its maximum curve with only time parameters.

  Reading the app settled it the other way. Its **commissioning** flow writes
  this field — an input widget bound to it, and a recommendation engine that
  computes the value from the largest supply pipe dimension and the pipe
  material. It is a flow limit for the recirculation loop, sized to the piping.
  The vendor's own setup flow computing and storing a value there makes it
  supported by any reasonable definition, whatever the settings screens show.

  Two things recorded so the next person does not repeat the search: it appears
  on no ordinary app screen (finding nothing is the expected result, not
  evidence the component invented it), and it survives normal app use — changing
  cycle times in the GO app preserves it. The docs previously ended on "treat it
  as a capability that happens to work rather than a supported one", which is
  now the opposite of what is known.

- **`CONFIG_CONFIRM_DELAY_MS` carries its measurement** (issue #250). The
  constant is unchanged at 1200 ms; what changed is that it is now measured
  rather than assumed. Rebuilt with the delay cut and `CONFIG_MAX_ATTEMPTS` at 0
  so no retry could mask the first readback: at **50 ms**, five writes settled
  five `accepted`, each carrying back the value just written, and the same at
  200 ms. A stale readback would have settled `rejected`, a silent one
  `timeout`; neither occurred in ten writes.

  So the failure mode the issue was filed about — an early readback settling
  `clamped`/`rejected` for a write that landed — is not reachable by reading
  early on this pump, and 1200 ms has ≥24× margin. Nothing argues for moving
  toward the app's 2500 ms.

  `SET_CYCLE_TIMES` was measured the same way and gave the same answer, so both
  callers of the constant are covered rather than one measured and one assumed.

  The comment records what the margin actually belongs to: settle is 0.7–0.8 s
  even at a 50 ms delay, because the write sequence is several round trips of
  its own. The measurement cannot separate "the pump applies instantly" from
  "the sequence is long enough that this delay is irrelevant", so shortening the
  sequence would require re-measuring.

- **`ready_recycle` is a bounded count, not off-or-forever** (issue #257). As a
  boolean it offered two shapes and the useful one was neither. The reporter's
  case is a bonded, connected link that just does not get all the way through
  the opening GENI reads: if that is a one-off glitch, one reconnect clears it;
  if it is not, another fifty will not clear it either — and each one takes
  another run at the encryption-on-open window that can erase a bond (#14).

  So `0` never recycles (unchanged default), and `N` recycles at most **N
  consecutive times** and then stops, leaving the fault standing for an
  automation to notice. The bound is judged against the consecutive counter,
  which the pump becoming ready resets, so an allowance is per episode rather
  than per boot: a link that recovers gets its full allowance again next time.

  `ready_recycle: true` still means unbounded, so no existing configuration
  changes behaviour. Booleans are validated *before* integers and deliberately —
  `bool` is a subclass of `int` in Python, so an integer-first validator would
  silently read `true` as the number 1 and quietly turn "recycle forever" into
  "recycle once".

  Giving up is reported distinctly from never having tried, because "it stopped
  trying" is exactly the state the reporter wanted to be able to see.

- **`enable_pairing` is now `initiate_pairing`, and the docs say what it cannot
  do** (issue #245). The old name reads as a property of the link — "this node
  will not bond" — and that is a guarantee no ESPHome component can make. When
  the pump initiates, `BLEClientBase::gap_event_handler()` has already consented
  on our behalf, unconditionally, before this component sees the event. A node
  with the option off was observed bonding **four times in twenty minutes**,
  252 ms after logging `Skipping encryption request - pairing disabled`.

  The component already knew this and said so in a code comment. What was
  missing is that nothing outside that comment did — the option name, the docs
  table and the base package all implied a guarantee that was never available.

  `initiate_pairing` names what is actually governed: our side of the
  negotiation. `enable_pairing` is still accepted and means exactly what it
  always meant; setting both to different values is refused rather than
  resolved, since there is no reading of that config that is obviously intended.

- **`alpha_hwr_base.yaml` no longer promises telemetry "without BLE
  pairing/bonding"** (issue #244). It could not deliver either half. It does not
  deliver *unbonded*, for the reason above. And whether an unbonded link works
  at all on this pump is still open: the sibling client records a measured
  ~1.8 s drop of an unbonded connection in seven places, traced to a debugging
  episode rather than assumed, and on the ESP32 the pump accepts the connection,
  completes discovery, accepts the CCCD write and hangs up at ~2.02 s — having
  received **zero** GENI frames across 90 measured cycles.

  The package now says it never *initiates* pairing, states the bonding caveat
  plainly, and asks anyone running it successfully unpaired to say so on the
  issue. The repo asserted the old claim in four places and had measured it in
  none.

- **`clear_vacation` clears every vacation covering now, not just one** (issue
  #290). `submit_set_vacation()` resolves through `find_free_single_event_slot()`,
  which prefers an **empty** slot and does not look for an existing vacation to
  replace — so setting a vacation while one is stored writes a *second* enabled
  Stop event. `clear_vacation` then cleared the best-ranked one, settled
  `accepted`, and left the pump holding itself off under the other.

  #267 fixed the *ranking* of stored vacations, and that ranking is correct for
  the question it answers. But a ranking orders a set; it does not bound the
  set's size. This is #267's own sentence — "cleared the finished one and settled
  `accepted` … while the pump was still holding itself off" — with "finished"
  replaced by "the other live one".

  Every enabled Stop event covering the current time is now cleared, in slot
  order, as one operation with **one** terminal event. The settle detail names
  the count and the slots (`cleared 2 vacations (slots 0, 1)`); a clear that
  fails part-way settles `rejected` and reports how many were cleared and that
  the rest still cover now, because at that point the pump is still held off and
  a bare rejection would not say so.

  **Creation is deliberately unchanged.** A second future absence is legitimate,
  and replacing one silently would destroy a stored event — the class of thing
  #262 exists to prevent. While a vacation is live, one that has not begun is
  left alone by a clear, so ending the current absence does not cancel next
  month's booking. When *nothing* covers now the resolver still falls back to
  the single-vacation ranking and clears the soonest upcoming one, which is
  unchanged from #267 and is what makes a future booking cancellable at all.

  One judgement stated rather than hidden: a single clear walks at most
  `MAX_VACATIONS_PER_CLEAR` (8) slots, because the watchdog cannot be re-armed
  once an operation is running and its budget therefore has to be fixed before
  the slots are resolved. It is not expected to bind — only vacations live at the
  *same instant* can be multiple, and they all have to fit in the pump's slots,
  of which this bench unit has five. If it ever does, the detail says how many
  are left and a second call finishes the job.

- **A wire-supplied length is no longer a loop bound** (issue #284).
  `EventLogService::read_entries_async()` decided how many entries to read from
  two `uint16_t` fields it had just decoded off the metadata reply, and nothing
  clamped either — so `count` could be **65,535**. That is roughly 1.9 hours of
  reads at the corpus's reply latency (91 hours if they go unanswered), ~512 KB
  accumulated in `cached_entries_` on a part with a fraction of that free, and a
  `uint16_t` sub-id that wraps from idx 55336 into a completely different part of
  object 88's address space. It also reintroduced #259: `abandon_queue_()` caps
  its unwind at 512 steps, so a disconnect during a longer chain strands the
  caller with no terminal callback.

  Two ceilings, and they say different things. The **address map** gives 1001:
  `geni_profile_52_7.xml` has `event_log_obj` at sub-ids 10200–11200, so reading
  past that is meaningless whatever the pump claims — a bound with no "is it
  enough?" conversation attached. The **binding** one is
  `Transport::MAX_ABANDON_STEPS`, since a chain longer than the unwind cap is
  exactly the hazard above; a `static_assert` now makes the two agree by
  construction rather than by comment, which is why that constant became public.
  Clamping is reported at WARN, never silent.

  Two more of the same shape in the same pass:

  - **A second chain can no longer be queued behind the first.** A clamp bounds
    one chain and does nothing about N of them, and the re-arm backoff bumps the
    read generation without stopping work already in the transport queue — so a
    slow read could be duplicated into an unbounded one. `read_entries_async()`
    now refuses while one is in flight, releasing the flag on every terminal
    path including the abandoned one.
  - **The single-event read is clamped to the address map.**
    `get_max_single_events()` returned `overview_structure_[1]` — a byte straight
    off the wire. `SINGLE_EVENT_SLOT_LIMIT` guarded the *write* path and was
    simply never applied to the read, the identical omission the sibling client
    found (eman/alpha-hwr#40). SubID is `900 + slot` and the schedule *layer*
    records start at 1000, so a pump reporting more than 100 sent the read chain
    into layer 0, reading schedule layers as though they were single events. The
    clamp moved into the accessor, where all three call sites share it — two had
    already drifted apart.

  Worth saying plainly: no pump has been seen reporting a wrong count on either
  side. Both bench units report 20 entries and 5 slots, so these bounds are
  unfalsifiable against real hardware and their whole value is in the shape.

- **A Class 10 read the pump declines completes at once instead of timing out**
  (issue #283). The pump answers a read it cannot fulfil with the same nine bytes
  a write acknowledgement uses — `24 05 F8 E7 0A 01 04 EE 26`, acknowledge OK at
  the head with `OPERATION_FAILED` below. That frame matched nothing:
  `short_ack_shape` requires the queued command to be a SET, and the `len < 11`
  floor dropped it. The read waited out its whole timeout and the log said "no
  response" about a pump that had answered in milliseconds — #208's defect, one
  operation across.

  Measured against the pump: an answered read costs 0.06 s and a declined one
  the full 3.00 s, and anything walking a sub-id range pays it per read — 54
  declined sub-ids in one limiter sweep, about 2.7 minutes of dead link. **The
  seconds are the smaller half.** A declined read and a silent pump produced the
  same result, and 54 of them were read as "the pump ignores these", which was
  wrong and reached a bench note.

  **The caller declares it.** #279 tried this as an unconditional branch and was
  withdrawn: `TelemetryService` queues its five Class 10 reads with no callback,
  so they never record a reply debt, and a refusal to one of them arrives
  orphaned while the next read is in flight — the branch then completed an
  innocent read as a failure, and did so most on exactly the pumps it was written
  for, since a pump that refuses reads is a pump that generates these orphans.
  The limiter and setpoint-range chains opt in; nothing else does.

  Three further constraints, each one #279 was withdrawn for missing: an upper
  length bound of 9 bytes, because byte 5 is the *first* APDU's length on a
  multi-APDU telegram and App C.17 makes "first APDU errored, second carries the
  answer" a documented case; `apdu_op(...) == GET` rather than
  `!apdu_is_set(...)`, since the negation also catches INFO; and the status byte
  read only when the head's acknowledge is OK, because otherwise that byte is the
  offending Data Item's ID. Added on top: a frame whose acknowledges are *both*
  OK is left alone — it is byte-identical to a legitimate one-byte data reply,
  and failing a read on it would trade three seconds for a lie.

  The refusal is handed to the callback as bytes where a timeout passes
  `nullptr`, so a caller can tell "the pump declined" from "the pump said
  nothing" with no change to the callback signature. A refusal arriving while an
  abandoned command is still owed one pays that debt first, exactly as the
  short-ACK branch does (#248), so one late reply costs at most one match.

- **`Control mode updated to ...` says what happened, and stops repeating it**
  (issue #265). The line lived in `get_mode_async()`'s readback handler — a read
  path — and fired on every successful readback, most of which are the periodic
  one driven by `control_state_poll_interval` where nothing changed at all.

  Two defects in one line. "Updated to" claimed a state change that had not
  happened, so a reader scanning for changes had no way to tell a poll from a
  real transition without diffing consecutive lines; it now reads `Control mode
  is 2 (Constant Speed), setpoint=1650.00`. And it was the highest-frequency line
  in a normal build — on an idle pump essentially the only recurring INFO line,
  twice a minute, about 2,880 a day, every one identical. It now logs at INFO
  when the mode or the setpoint moved and at DEBUG when neither did, which is the
  same argument `publish_gate.h` already makes one layer over (issue #127); the
  gate had been applied to the entity publishes and not to the log.

  The first reading after a connect counts as a move, so the state still reaches
  INFO once per link. The comparison treats NAN as a value rather than a missing
  one — `get_setpoint_for_mode()` returns NAN for every mode with no scalar
  setpoint, and a plain `!=` would have reported a change on every poll of
  exactly those modes.

- **Match-failure logs name the object type and version instead of two
  byte-pairs labelled as neither** (issue #281). `Object %d SubID %d` was wrong in
  both halves and the numbers were not a type either: a reply carries no Object ID
  and no Sub-ID, and the two 16-bit values the matcher holds split the reply's
  `[00][TypeH][TypeL][Version]` header one byte off that boundary.

  The split is correct and stays — comparing both halves is equivalent to
  comparing type and version together, so matching is unaffected. What it is not
  is a pair of numbers anyone can look up. `Object 55809 SubID 0` is verbatim the
  line quoted in #253 while a real bug was being chased; it is type 218 v1,
  `ClockProgramOverview`, and there is no Object 55809.

  New `protocol::apdu_object_type()` / `apdu_object_version()` accessors decode at
  the real boundary, and everything a human reads goes through them — the command
  timeout warning, the quiet-timeout line, the `[AWAITING]` trace, and the three
  lines in the (callerless) `pending_handlers_` path the issue named. The pair
  form is left in the matcher, where it works, with a note at the field
  declarations saying what it is not. The sibling Python client made the same fix
  (eman/alpha-hwr#39).

- **A single-event window that has already ended is refused** (issue #269).
  `run_single_event_()` validated only that the end was after the begin, so a
  window entirely in the past was written to the pump, confirmed by readback,
  and settled `accepted` — after which it was recyclable garbage occupying one
  of five slots. Observed on the bench while verifying #262: two events written
  with yesterday's window both landed and both confirmed.

  Not destructive, and arguably harmless, since the pump simply never runs it.
  But it is a write that cannot do anything, reported as a success: a client with
  a timezone bug or a stale timestamp got `accepted` and no signal.

  Only the **end** decides. A window that has begun but not ended is legitimate
  and still runs — refusing it would break every "start this now, stop it at six"
  request, which is what the Lovelace card's Quick Run presets send. With no
  synced clock the write is not refused on these grounds; the same rule #262
  established for the slot picker, where an unknown clock expires nothing rather
  than everything.

  The refusal settles `invalid` ahead of the schedule-overview read and quotes
  both the window's end and the node's clock, so a client with a timezone bug can
  see which timestamp it sent.

  **Withdrawn from #287 and restored here.** The check was correct; its inputs
  were not. `op->end_ts` can come from `build_event_window()`, which used
  `mktime()` — which ESPHome does **not** override on the ESP32 (unlike
  `localtime_r`; see #289 and its correction), so that timestamp and
  `now_unix()` were in different bases and every window ending inside the node's
  UTC offset was refused. #289 put both on `ESPTime`; the check needed no
  change, only a floor to stand on.

- **Local time now comes from ESPHome's timezone engine, not from libc — which
  fixes single events firing at the wrong hour on every non-UTC node**
  (issue #289).
- **Local time conversions go through ESPHome's timezone engine, which fixes the
  schedule editor's dated events and two timestamp displays** (issue #289).

  > **Correction.** An earlier draft of this entry claimed libc had no timezone
  > at all on the ESP32, that the single-event wire shift was therefore a no-op,
  > and that stored events needed re-entering. **All three were wrong.** ESPHome
  > *does* supply local time to libc callers on embedded targets — it overrides
  > `localtime_r()` and `localtime()` in `posix_tz.cpp` to use its parsed zone,
  > precisely so that user lambdas calling `::localtime()` work without the `TZ`
  > environment variable. What follows is the corrected account. **No stored
  > event needs re-entering.**

  What was actually broken is narrower, and it is what `mktime()` touches —
  because `mktime()` is **not** among the functions ESPHome overrides, so on the
  device it resolves against a libc that genuinely has no zone:

  - **`build_event_window()`** encoded local calendar fields as though they were
    UTC. This is the schedule editor's "Add Single Event" and "Set Vacation"
    buttons, so a window entered through them landed on the pump offset by the
    node's UTC offset. Events submitted through the **services** with explicit
    epochs were never affected.
  - **The event-log and cycle-timestamp displays** were shifted twice. Those
    timestamps come off the wire raw and are already the pump's local clock, so
    rendering them "to local" moved them again by the offset.

  Everything else that used libc — the single-event wire shift
  (`local_utc_offset_seconds()`), the vacation and single-event displays, and the
  DST probe — was **already correct on the device**, via the override above. Those
  sites moved to `ESPTime` anyway: it is the engine that is right on both targets
  and does not depend on a shim, and having one answer to "what is local time"
  is the same argument as issue #270's.

  The mock's `ESPTime` gained an embedded mode (`MockZoneOverride`) so a
  conversion done the wrong way is visible on the host, where otherwise both
  engines agree and nothing can discriminate.

- **A vacation that has already ended no longer shadows the live one**
  (issue #267). `find_vacation_slot()` returned the first enabled Stop
  single-event in the cache, in slot order, with no reference to a clock. With a
  finished vacation in slot 1 and a live one in slot 3, `clear_vacation` cleared
  slot 1 and settled `accepted` — the user was told the vacation was ended while
  the pump was still holding itself off. `find_free_single_event_slot()` one
  method up had always been clocked; the asymmetry was the whole bug.

  `format_vacation_display()` had the same shape, which is why the same defect
  showed up twice: the **Vacation** text sensor named an expired vacation as *the*
  vacation. Both now go through one ranking, so the rule cannot drift apart
  again.

  The preference is: a vacation whose window covers now, then the next one due
  (soonest begin, not lowest slot), then the one that ended most recently. The
  third case is included deliberately rather than by omission — an ended
  vacation is still an enabled Stop event occupying one of the five slots this
  pump has, and refusing to clear it would leave no way to reclaim it. It logs
  that it fell back. The **Vacation** sensor does not show that case at all: the
  sensor answers "is the pump being held off, and until when", and a window that
  closed last month answers that with "no".

  With no synced clock the first stored vacation is returned unranked, as before
  — a picker that cannot tell the time does not get to decide which of two
  vacations has ended.

- **A single-event window the pump cannot store in local time is refused, not
  wrapped** (issue #263). The pump's clock program stores single-event
  begin/end as **local** Unix time, so every timestamp is shifted by the local
  UTC offset on its way to the wire and back. That shift wrapped modulo 2^32 at
  both ends of the range the wire carries.

  What makes it an issue rather than a footnote is that the two wraps **cancel**.
  The confirm readback shifts back by the same offset, wraps symmetrically, and
  compares equal — so the operation settled **accepted** for an event the pump
  had stored at an instant nobody asked for. A byte round trip is not behaviour,
  and here the round trip was supplied by two wraps agreeing with each other.
  There is a second failure at the ceiling: a correctly ordered pair whose end
  is within the offset of `UINT32_MAX` arrives at the pump **reversed**, because
  both the ordering check and `SingleEvent::is_valid()` run before the shift and
  nothing looked after it.

  The usable range is `[|min_offset|, UINT32_MAX - max_offset]` — at ±14 h, about
  a day's worth of instants out of 136 years — so the likelihood is low. A
  low-likelihood failure that reports success is still worth a guard.

  The two directions are now guarded differently, on purpose. **Encoding
  refuses**: the caller named an instant this pump cannot store in this zone, and
  writing a nearby one instead is exactly the silent substitution above. The
  refusal settles `invalid` ahead of the schedule-overview read, so an argument
  that is wrong regardless of the device is not reported as a link failure.
  **Decoding saturates**: the value is already on the pump — written by the GO
  app, or left by a firmware we never spoke to — so there is nothing to refuse,
  and the closest representable UTC keeps a pair ordered and keeps "has this
  ended?" answerable, where wrapping moved the instant 136 years and made an
  expired event read as live.

  Settled in the same change: `0` is the wire's disabled/cleared sentinel and is
  never shifted, so an enabled event whose begin was `0` confirmed clean while
  describing a slot that says "cleared". The service argument parser now floors
  both epoch fields at 1, and `set_single_event` with `0,4294967295` moves from
  the accepted table to the rejected one.

- **One accessor answers "what time is it", at one sanity floor** (issue #270).
  The component resolved the node's wall clock in five places, each with its own
  source and its own idea of when a clock is trustworthy: `TimeService` floored
  the year at 2021, `build_event_window()` floored it at 2020, and the two drift
  measurements and the vacation check tested against the literal `1609459200`.
  Three floors, two of which agreed by coincidence rather than by construction,
  and none of which referenced the others.

  Three of the five read `::time(nullptr)` directly, which is where the divergence
  had teeth. Under `#ifndef USE_TIME` those three fell through to libc while
  `TimeService` declined to answer — so the same build had one subsystem refusing
  to act and three acting on a clock nothing had validated, in a zone nothing had
  loaded. A build with no time component has no timezone, so libc's answer there
  is not merely unvalidated; it is in the wrong base, against a pump whose clock
  program runs on local time.

  All five now ask `TimeService`, which answers in whichever of three shapes the
  caller needs — calendar fields, epoch seconds, or "is there one yet" — from a
  single read, so they cannot disagree. `CLOCK_SYNCED_YEAR_FLOOR` is the only
  floor left. Under `#ifndef USE_TIME` every caller refuses, uniformly.

  Nothing here was known to be broken, and the change is filed as prevention
  rather than repair: issue #262 was caused by one caller substituting the wrong
  timestamp for "now", and several independent notions of now is the condition
  that makes that class of bug easy to reintroduce.

  Two behaviour changes are worth naming. `build_event_window()` — the schedule
  editor's "Add Single Event" and "Set Vacation" buttons — now refuses a clock
  below year 2021 where it previously accepted year 2020, and refuses outright in
  a build with no time component where it previously anchored the event to
  whatever libc said. And the `Last Clock Sync` stamp's libc fallback is deleted
  rather than converted: it was unreachable (the clock-sync gate will not submit
  a sync without a synced clock at all), and what it did when reached was stamp
  the sensor `1970-01-01`.

  The accessors are pinned by a new host test built **twice**, with and without
  `-DUSE_TIME`, because "the behaviour under `#ifndef USE_TIME` is the same for
  every caller" is not checked by anything if that build is never built.
- **A setpoint outside the pump's range is clamped by the pump and explained,
  not refused before the wire** (issue #276). This takes back out the check
  #273/#275 added, and the reason is worth recording because the bounds it
  validated against were *correct*.

  @jfriend00 found the flaw: **with a flow limiter enabled there is no maximum
  speed.** The pump takes the setpoint and manages actual run speed to hold the
  flow bound, and where it lands is a property of the loop's hydraulics rather
  than of the pump. Measured on their installation, constant speed with MaxFlow
  at 1.6 gpm:

  | commanded | delivered |
  | --- | --- |
  | 1700 RPM | 1701 |
  | 1900 RPM | 1903 |
  | 2000 RPM | 1892 |
  | 3000 RPM | 1883 |

  1883 RPM is not in the type-301 range, not in the limiter record, and not
  anywhere else — the pump discovers it by running the loop, and does not know it
  in advance either. So there is no number to narrow to, and a check that *looks*
  authoritative is worse than no check, because it is wrong in a way no client
  can detect.

  The type-301 range is therefore not "the bound"; it is "the bound in the
  absence of a limiter". It now survives as an **explanation** rather than a
  gate: when the pump clamps, the settle detail says so and quotes the range —

  ```
  clamped: pump stored 3671; its range for this mode is 1650-3671 RPM
  ```

  — and quotes it **only when the pump is the source**. The fallback constants
  this code used to carry were wrong in both directions on all four modes, so
  printing one as though the pump had said it would turn an explanation into a
  fabrication.

  This also settles the slider question the issue was filed for, by removing it:
  if writes clamp instead of being refused, no slider offers a value that gets
  refused, so nothing has to track the pump's bounds at runtime and the Home
  Assistant entity-registry problem goes away with it. The behaviour a user sees
  is the one that was there before #275 — drag the slider, watch it settle on
  what the pump could do — with the settle event now saying why.

  One thing is still refused before the wire, for a reason that is not about
  range: a setpoint that is **not a number**. There is nothing for the pump to
  clamp to, and the all-ones float doubles as the `SETPOINT_KEEP` sentinel on the
  wire, so a NaN would read as "leave the setpoint alone" — a write that silently
  does nothing rather than one that fails.

- **Every Class 10 write now waits for its own acknowledgement** (issue #253).
  This finishes what #248 began. That change stopped a reply being handed to
  whichever write happened to be waiting; it did not stop replies being left with
  nobody waiting for them at all. Four sends still went out with no callback —
  the fused Obj 0601 control request, the OpSpec 0x88 setpoint register write,
  the Obj 94 clock write and the ClockProgramOverview commit — and a send with no
  callback never enters the transport's response state, so it never times out,
  so it never records the reply debt that was supposed to catch a stray answer.
  Auditing for those turned up three more, described below, that were waiting for
  the wrong thing rather than for nothing.

  **There is nothing in a reply to correlate with.** Across
  `resources/traffic_capture`, reassembled and de-duplicated, all 195 Class 10
  SETs in 20 distinct address shapes — clock, schedule layers, overview commit,
  control request, mode write, temperature range, DHW config — are answered by
  the *same nine bytes*, `24 05 F8 E7 0A 01 00 AE A2`. Not one bit distinguishes
  which write is being acknowledged. The specification says so in advance: *"the
  SET operation never returns anything but the APDU Head"* (Application
  Programmers Manual, fig 3.5 note 1). Correlation is positional or it does not
  exist.

  So the wait is the only mechanism available, and it is cheap: 400 ms against a
  worst case of 193 ms for these shapes and 295 ms anywhere in the corpus. None
  of these callers treats the acknowledgement as its verdict — every one confirms
  by reading the value back — so the wait exists to *spend* the reply, not to
  learn from it.

  **The short-ACK branch is gated on a declaration rather than a list of
  addresses.** It carried five hard-coded address shapes, which is why closing
  these sends had been deferred: each conversion meant remembering to add a row.
  The list could not have been doing the job anyway — it inspected the queued
  command, so it never narrowed *which* write a reply answered, only which writes
  were allowed to be answered at all. It had already accumulated one dead row for
  a frame nothing builds and one added speculatively for a send with no callback
  to use it. What admits a frame now is `expect_short_ack`, the caller's own
  statement that it is awaiting exactly this reply — strictly narrower for
  anything that does not opt in.

  **And a three-second stall on every Object 84 write, with a false explanation
  attached.** Three more writes — the schedule layer image, the schedule
  enable/disable, and the single-event slot — were not fire-and-forget at all.
  Each awaited a reply carrying a *type* (`0xDE01`, `0xDA01`, `0xDC01`), which a
  SET reply cannot carry. So each timed out on every single attempt, with
  `quiet_timeout` keeping that at DEBUG and the callback reporting success from
  the timeout path. Two comments explained the silence — the pump "commits on
  timeout", the pump's "two-phase commit often closes the window without a
  matchable ACK" — and both were written backwards from a symptom the code was
  causing. The captures contradict them directly: 20 layer writes and 34
  overview writes, every one answered in 36–193 ms.

  Two of the three did not even time out quietly. `quiet_timeout` was set on the
  layer write but not on the schedule enable or the single-event write, so those
  two took the warning branch: every schedule enable/disable has been logging
  `Command timeout waiting for Obj 55809 Sub 0`, and every single-event write
  `Obj 56321 Sub 0`, once per write, for a reply that was already sitting in the
  log a few lines above. Anyone who has looked at a schedule change in the debug
  log has seen this.

  Each of those writes now settles in tens of milliseconds instead of three
  seconds. A five-layer schedule upload was spending fifteen seconds waiting for
  replies it had already been sent. Worse than the delay, since #248 each bogus
  timeout was recording a reply debt, so a write that had been answered promptly
  was also, on paper, owed a reply — and the next Class 10 write within the
  stale-reply window paid for it.

  **The settle window is now measured rather than inherited.** Those confirm
  readbacks are scheduled *from the write's callback*, so the 3 s timeout had
  been silently acting as part of the settle window — the real interval between
  a schedule write and its readback has always been 4500 ms, not the 1500 ms
  written down. Rather than guess which part of that the pump needed, a probe
  build set the settle to 100 ms with a 200 ms retry ladder and wrote to a spare
  schedule layer: **four writes, set and clear, and the first confirm read
  matched every time.** The pump makes an Object 84 write visible to a read
  within 100 ms of acknowledging it.

  So `SCHED_SETTLE_DELAY_MS` stays at the 1500 ms it always claimed — now 15×
  a delay shown to be sufficient — and the schedule-enable confirm shares that
  constant instead of carrying a literal of its own. With the retry behind it
  the ladder covers 3500 ms before any `rejected` verdict, comfortably past the
  2500 ms the Grundfos GO app holds the bus quiet after a SET.

  The measurement is scoped, and the constant says so: it was taken on the layer
  image, applied to the overview path by inference, and says nothing about the
  Obj 91 config writes — `CONFIG_CONFIRM_DELAY_MS` is what issue #250 is about
  and is untouched here.

  The test simulator was modelling the same false belief: it echoed the object
  back after these writes, because that is what the firmware asked for. It now
  answers them the way the pump does.

  One honest limit: the `0x88` setpoint register write is the one converted send
  whose exact frame the corpus does **not** contain — the Grundfos GO app sets
  setpoints through the fused control request instead. It rests on the class-wide
  rule and the specification, and on bench verification, rather than on a
  captured instance of itself.

- **A reply is no longer given to whichever write happens to be waiting**
  (issue #248). GENIbus replies carry no sequence number and no object echo, and
  the specification is explicit about the consequence: *"the Data Reply is not
  self contained, meaning that the Data Request is necessary to process it"*
  (Application Programmers Manual, fig 3.5). Correlation is positional, and it
  works only because the bus is interlocked — the reply follows within 50 ms and
  the master idles before the next request. Two requests are never outstanding,
  which is why no identifier exists.

  `run_set_temperature_range_` broke that. It sent the unfused mode write with no
  callback, so nothing waited for its reply, and 400 ms later sent the config
  write that *does* wait — same class, and the earlier reply is byte-identical to
  the acknowledgement the later write expects.

  Two guards now compose. **The mode write is awaited**, with a wait equal to the
  step-2 delay so an answered write frees the queue at the observed p50 of 54 ms
  and an unanswered one expires exactly when step 2 was due — it costs nothing in
  either direction. And **the transport records a reply the pump still owes**:
  any command that gives up leaves a debt, the next ambiguous frame settles it
  rather than being taken for an answer, and the debt expires if never paid.

  The debt is a *count*, not a deadline, and that is the design rather than an
  implementation detail. A first attempt suppressed on a time window alone and
  cascaded: a suppressed frame left its command to time out, that timeout re-armed
  the window, and the next acknowledgement landed inside the new one — four
  consecutive writes failing against a perfectly healthy pump. Paying the debt
  down, and not letting an already-suppressed command open a fresh one, is what
  makes it terminate. Damage is bounded at one lost acknowledgement per genuinely
  late reply.

  Coverage has a stated bound: a mode reply is attributable out to 900 ms, the two
  guards composed. That is three times the slowest reply the captures contain
  (295 ms across ~12,000 pairs; nothing exceeds 400), so the uncovered band is
  populated by no observation. Widening is nearly free under a debt — it is spent
  once, not per frame — so a pump seen replying later wants a larger window, not a
  redesign.

  Not fixed here: `send_control_request`, `set_class10_setpoint`, the clock write
  and the ClockProgramOverview commit still fired and forgot. Issue #253 tracked
  those and closed them; see the entry above.


- **A Class 10 reply carries two acknowledgements, and both are now read.** The
  APDU head says whether the request was understood; Class 10 then adds a status
  byte of its own — `OK` / `BUSY` / `OPERATION_FAILED`. Only the head was being
  read, so a pump answering "busy" or "that failed" would be reported as a
  successful write.

  Named by the Grundfos GO app's own decoder (`GeniAPDU.CLASS10_ACK_*`, read from
  the byte after the head — the only source that documents it; neither the
  specification's Class 10 reply diagram nor the public GENIbus libraries have
  it), and present in `resources/traffic_capture` with exactly those three values
  and no others: 222 short Class 10 replies, 195 `OK`, 18 `BUSY`, 9
  `OPERATION_FAILED`, every one with head acknowledge `OK`. (These counts were
  first published as 459/420/26/13, which a recount for #253 could not reproduce
  at any level of de-duplication — 222 with the ten unique files, 287 with all
  fourteen. The three-way split and every conclusion drawn from it stand; only
  the totals were wrong.)

  **Defensive rather than a live fix, and worth being exact about which.** Every
  non-`OK` value in the corpus answers a *read*, and reads carry Obj/Sub so they
  never reach this branch; all 195 captured write acknowledgements carry `0x00`.
  So no write has been misreported. What the change buys is that one no longer
  can be. The two objects involved are firmware-update related
  (`FwUpdate_ECUOverview_obj`, `FwUpdate_SetECUInFocus_obj_2`), and one of them
  answers `BUSY` on half its reads and real data on the other half — a value that
  alternates with data is a status, which is what settles the reading.

  Also corrected while in this branch: the payload byte was read at `len >= 7`,
  where a real short acknowledgement is 9 bytes. On an 8-byte CRC-valid frame
  declaring one payload byte, that read the **CRC high byte** as the status — and
  since the status now decides the verdict rather than the wording of a log line,
  the bound matters.

- **`build_geni_packet` could write past its caller's buffer.** GENIbus caps a
  telegram at 259 bytes and its PDU at 253; the guard tested `length > 255` — the
  widest value the length byte can hold — and a frame is `length + 4` bytes, so
  an accepted length of 255 wrote **259 bytes into the 256-byte buffer**
  `send_apdu_command` declared.

  Latent: the largest APDU anything builds is the 53-byte schedule write, so no
  call has come close. Found by reading the specification's size table rather
  than by hitting it, and it reproduces as a stack-buffer-overflow under ASan and
  as a stack-canary abort at plain `-O2`. The cap is now the protocol's
  `MAX_PDU_LEN` and the buffers are `MAX_TELEGRAM_LEN`, because even a legal
  maximum telegram did not fit in 256 bytes.

  `resources/traffic_capture/README.md` records how to read that corpus without
  reaching false conclusions from it — ATT reassembly, and the three files that
  are the same session.

- **A config write the pump stored but did not acknowledge no longer settles
  `rejected`** (issue #234). `set_temperature_range` and `set_cycle_times` both
  short-circuited to `rejected` when no acknowledgement arrived inside the
  write's 3 s window — before any readback ran. `rejected` asserts the pump did
  not take the write, and nothing at that point knew it: the only evidence was
  that no frame had come back. A write that landed and whose acknowledgement was
  lost — a dropped notification, a reassembly failure, a frame a moment late —
  was reported as a failure while the pump held exactly what was asked for, and
  an automation retrying on `rejected` would rewrite values that were already
  correct.

  The acknowledgement could never have carried that weight. The Class 10 short
  ACK has no sequence number and no object echo, so the transport matches it
  against "some queued command of this shape" — and `set_temperature_range`
  sends a fire-and-forget mode write 400 ms ahead of the write that carries the
  callback, on the same class, against reply latencies observed at 250–360 ms.
  Issue #233 closed the half of this that a *reply* could cause; silence is the
  other half, and is less attributable still, there being no frame to reason
  about at all.

  Both writes now defer to the readback that was already there, so the verdict
  comes from what the pump reports holding: the requested values settle
  `accepted`, different values `clamped`, and the old values `rejected` — a
  refusal is still a refusal, and still distinct from a clamp. Reaching the
  readback also meant giving `set_temperature_range`'s confirm the
  kept-versus-clamped distinction that `set_setpoint` and `set_cycle_times`
  already had; it had only two outcomes, so a pump that ignored the write
  reported "the pump stored something else" about a pump that had stored
  nothing.

  The silence is not lost, only demoted: every settle from an unacknowledged
  config write carries `config write not acknowledged` at the front of its
  `detail`. That makes this the one case where an `accepted` settle has a
  non-empty `detail`, which is documented in
  `docs/programmatic-interface.md` along with the full mapping.

  `set_temperature_range` now **reads its config object before writing it**, as
  `set_cycle_times` already did. The values a verdict is measured against have
  to come from the pump, and the cache could not supply them: `read_obj91_config`
  runs once per connection, in the initial read chain, and is not in the periodic
  control poll that refreshes the mode and setpoints (issue #54) — so on a link
  up for hours the baseline is that old. A Grundfos GO app edit in between would
  make a write the pump *ignored* report `clamped`, blaming the pump for a choice
  it never made. The same read refreshes the pump's own on/off-time limits, which
  that write echoes back verbatim (issue #106) and had previously been echoing
  from a connect-time capture.

  Both writes also get their own watchdog budget, 26 s instead of the 10 s
  default, sized to the whole path: pre-write read, ACK window, confirm readback,
  retry, second readback. On the old budget the watchdog fired before the
  confirm's retry could send a second readback, so `CONFIG_MAX_ATTEMPTS` never ran
  and the confirm's own "readback failed" verdict was unreachable. That was
  survivable while a missing ACK settled at 3.4 s without any readback; now that
  the readback decides, a write the pump stored but did not acknowledge — whose
  first readback is then dropped — has to reach the retry to be reported as the
  success it is, rather than trading one wrong failure for another.

- **A mutation that hangs a test is reported instead of stalling the sweep**
  (issue #237). `tools/mutation_check.sh` had three outcomes — caught, survived,
  and did-not-compile — and a mutation that made a test loop forever produced
  none of them. The run simply stopped advancing: no output, because progress is
  printed per entry; the script at 0% CPU with a test binary at 99%; and nothing
  naming the entry, because the mutated file is restored per entry and the name
  lives only in the script's memory. It reads exactly like a slow sweep, and one
  such mutation cost 34 minutes of silence before anyone noticed the log had
  stopped growing.

  Each suite run is now bounded, and a run that blows through the limit is its
  own outcome: named, listed separately from the survivors, and fatal. The two
  are different — a survivor is an answer, a hang is the absence of one — and
  the hang is the more urgent, because until it is fixed every later entry is
  delayed behind it. The baseline is bounded too, where an unbounded loop in an
  *unmutated* test hangs before a single mutation is applied.

  The limit is deliberately huge rather than tight: 300 s against a suite that
  runs in about 7 s. A false timeout would blame the tool for someone's correct
  change; a real hang caught late costs five minutes instead of a day.
  `TEST_TIMEOUT` and `BASELINE_TIMEOUT` override it. Implemented by hand rather
  than with `timeout`, which is GNU coreutils and absent on macOS — and the kill
  targets the process group, without which the runner dies and the spinning test
  binary survives as an orphan.

- **`lint.sh` fails when cppcheck did not run, instead of reporting a pass.**
  Found by shellcheck while fixing the above: the script captured cppcheck's
  exit status and never read it, so a cppcheck that failed to start — a bad
  argument, a glob matching no files — printed `✓ Analysis passed` over an
  analysis that never happened. The cppcheck CI job is a single call to this
  script, so the gate reported success exactly when it had stopped being a gate.
  The status is now fatal when nothing at all was counted; it cannot fire on a
  real finding of any severity, so nothing that passes today starts failing.

  `shellcheck --severity=style tools/*.sh` now runs in CI. All three scripts are
  clean; two of them decide whether a change may merge, so a shell bug in them
  does not give a wrong answer, it gives a check that quietly stops checking.


- **The packages' component source is a substitution, so a config can redirect
  it instead of declaring a second one** (`component_source`). Every package
  ships an `external_components` block pinned to its release tag. A config that
  wanted a different source — the CI harnesses pointing at the working tree, or
  a user tracking `@main` — added a second block, and both then resolved: the
  pinned repo was still cloned on every run.

  Which source actually supplied the component was never stated anywhere. It
  fell out of ESPHome merging the package's list *before* the config's own
  (`merge_config` returns `old + new`, packages being `old`) and
  `install_meta_finder` inserting each source at the front of `sys.meta_path` as
  it walks that list — so the last one processed wins, and the working tree won
  only because it happened to be last. Correct today, silent if it ever
  inverted, and paid for with a clone nobody used.

  `tests/ci-compile.yaml`, `tests/ci-compile-base.yaml` and
  `tests/ci-compile-schedule.yaml` now set `component_source: ../components` and
  declare no `external_components` of their own, leaving exactly one source in
  the merged config. Nothing is cloned, and CI compiling the branch under test
  is now a property of the config rather than of loader ordering.

  The substitution takes a plain string and ESPHome's shorthand source validator
  tries a filesystem directory before the `github://` pattern, so one key covers
  both forms. Unchanged for anyone not setting it: the five example configs
  still resolve to `@v0.15.0`, and `tools/bump_version.sh` still rewrites the
  pin, which now lives in the substitution.

- **The host test suite compiles once per translation unit instead of once per
  target.** A full build compiled 142 translation units out of ~40 distinct
  files — `codec.cpp` fourteen times, `transport.cpp` ten — because every target
  was a single compiler invocation over its whole source list, with one coarse
  wildcard standing in for header dependencies.

  That was survivable until `test_component_wiring` and `test_api_bridge`
  arrived. Each compiles 21 translation units and both include `alpha_hwr.h`, so
  most mutation entries select at least one of them, and a full
  mutation sweep grew to the better part of an hour — long enough to stop being
  something anyone runs before pushing.

  Objects are now cached under `.obj/<flags-hash>/<group>/`, `-MMD` records the
  real include graph, and `tools/mutation_check.sh` builds at `-O0` and deletes
  exactly the objects whose recorded dependencies name the file it mutated.
  **A full sweep went from about 36 minutes to 7, with every mutation still
  caught.**

  The flags hash is load-bearing rather than tidiness: make cannot see that
  flags changed, so without it `make OPT=-O0` would link `-O2` objects and
  `test-asan` would reuse un-instrumented ones and report a clean run against
  code the sanitizer never saw. Objects for `-DUSE_TIME` and `-DUSE_TEXT_SENSOR`
  are kept in separate groups for the reason AGENTS.md §4 gives — those defines
  compile a different program, and sharing an object across them would report
  guards as covered that were never built.

  One bug this shook out on the way: `tools/mutation_targets.py` read a target's
  sources off the line that produced it, which found nothing once the build had
  a link step, so every run fell back to rebuilding everything. It was loud in
  the output and annotated CI, but it silently cost the entire speedup until it
  was noticed.

- **The gap sampler no longer measures across downtime when a notification
  arrives with no connection behind it.** It arms from that frame instead. No
  path delivers one today, but the consequence changed with the histogram: an
  inflated maximum is one number a reader already knows is a floor, while an
  inflated sample permanently increments the top counters and reads afterwards
  as a genuine multi-minute excursion — an error in the direction that argues
  for keeping a default nobody has validated. It arms rather than discarding,
  so if such a path ever does appear it loses at most that session's first
  interval rather than all of them.

- **The transport's send-failure branches are tested.** Every write callback in
  the suite returned `true` unconditionally, so the two paths production takes
  when a chunk cannot reach the BLE stack had never executed under test: a GATT
  write returning false — the ordinary consequence of a link dropping mid-write
  — and a transport whose write callback was never wired (testable, though not
  reachable in production: the writer is always set before `loop()` can run).

  Both are correct. Four tests hold them there, pinned by four
  `mutation_check.sh` entries, asserting that the caller is failed immediately
  rather than left to wait out its own timeout, and that the queue advances so
  one lost write does not wedge the link for every command behind it.

  That second property took two attempts, which is the part worth repeating.
  The first version counted writes — and a count cannot tell the next command
  going out from the failed one being re-sent, so five assertions passed
  against a transport whose queue never advanced. It asserts on the payload
  byte now, and the `pop_front()` has a mutation entry of its own.

- **`alpha_hwr_controls.yaml` and `alpha_hwr_schedule.yaml` are documented as
  mutually exclusive, because they are.** The README said to "avoid combining
  both unless you want duplicate controls", which reads as a matter of taste. It
  is not: a config with both does not build. ESPHome stops at
  `Duplicate switch entity with name 'Schedule Enabled'`; fix that and
  `Duplicate select entity with name 'Pump Control Mode'` appears; fix that and
  `ID pump_mode_select redefined!` appears. Two conflicting entities, three
  errors. Renaming all three *does* yield a valid config, but only by forking one
  of the packages to run two overlapping sets of the same controls — so the
  README now says which to pick and why rather than how to merge them.

- **Two packages had no working-tree check at all**, which is why the above went
  unnoticed. `tests/ci-compile.yaml` loads `alpha_hwr_controls.yaml`, and the
  examples that load the other two pin a release tag — so CI was validating the
  last *release* of `alpha_hwr_schedule.yaml` and `alpha_hwr_base.yaml` rather
  than the branch. Neither can share a config with what is already covered:
  the schedule UI collides as above, and `alpha_hwr_base.yaml` is a parallel copy
  of `alpha_hwr_pairing.yaml` rather than an include. Both now have their own
  (`tests/ci-compile-base.yaml`, `tests/ci-compile-schedule.yaml`).

  The schedule UI is also **compiled**, not merely validated, because
  `esphome config` does not compile lambda bodies. Its select is the only YAML
  lambda in the tree naming `ControlMode::AUTO_ADAPT`, `AUTO_ADAPT_RADIATOR`,
  `AUTO_ADAPT_UNDERFLOOR` or `AUTO_ADAPT_COMBINED` — those four are all over the
  compiled C++, so renaming one and letting the compiler find the callers fixes
  the component and `alpha_hwr_controls.yaml`, and leaves this package quietly
  broken. That is a second full ESP-IDF build rather than a marginal one, since
  the build tree is not cached; the base package gets validation only, its single
  lambda being covered by the builds that already run.

- **The 0.3 GPM `flow_threshold` floor is settled by measurement, and stays**
  ([#180](https://github.com/eman/esphome-alpha-hwr/issues/180)). `AGENTS.md`
  §11.4 asked for one specific analysis before anyone touched the floor: the
  distribution of pump-off meter readings in the 0.05-0.30 band, split by
  proximity to a pump-off edge. That analysis now exists, over 10.4 days of one
  installation - 2,646 household-meter samples, 1,306 with the pump confirmed
  off, 81 pump-off edges, against the 14 h that opened the issue.

  Both halves of the original hypothesis hold, and together they argue for
  leaving the floor alone. Shutdown decay is real: the sub-threshold band is
  3.2x enriched within 30 s of a pump-off edge (33.1 % against a 10.3 % base
  rate), while the bands *above* the floor are depleted near edges at 0.71x and
  0.49x - decay lands below 0.3 specifically, which is what the floor is
  keeping out. But the genuine remainder turns out to be almost entirely
  demand that is already detected: **89 % of it falls within 60 s of a reading
  that already exceeds 0.30**, the ramp-up and tail-off shoulders of draws the
  detector has already declared. Truly isolated sub-threshold draws run to one
  episode in 10.4 days, and a lower floor buys a median of 0 s in onset lead.

  So the issue's premise - 24 sub-threshold pump-off samples read as discarded
  demand - was a sample-count argument. At episode level over 18x the window,
  lowering the floor would admit a measurable false-positive population to
  recover roughly one short draw per ten days. No code changes; the floor stays
  at 0.3 on evidence rather than by default, and both the AGENTS.md note and the
  `docs/configuration.md` row now record the numbers so this is not re-litigated
  on intuition.

- **BREAKING — the six pump services are renamed to match the `command` their
  settle event reports** ([#159](https://github.com/eman/esphome-alpha-hwr/issues/159)):

  | Old service | New service |
  | --- | --- |
  | `pump_set_enabled` | `set_pump_enabled` |
  | `pump_set_mode` | `set_mode` |
  | `pump_set_setpoint` | `set_setpoint` |
  | `pump_set_temperature_range` | `set_temperature_range` |
  | `pump_set_cycle_times` | `set_cycle_times` |
  | `pump_set_state` | `set_pump_state` |

  You called `pump_set_state` and got `command: "set_pump_state"` back, so the
  one field meant to tell you which write had settled named it differently from
  the thing you called — you could not grep a call to its own event, and the
  transposition reads as a typo in either direction.

  **The event payloads do not change.** Only the service names move, and they
  move onto the names the events were already using. This is the direction that
  breaks loudly: a renamed service fails the automation with `Action ... not
  found`, whereas renaming the event field would have left every
  automation filtering on `command` silently never firing again. Same rule as
  the keys retired in #149 — a surface that validates but does nothing is a
  trap.

  **Migration:** rename the service in each automation per the table above.
  There is no single rewrite rule to apply — `pump` is *dropped* where the verb
  alone is unambiguous (`pump_set_mode` → `set_mode`) and *kept* only where it
  says which thing is being set (`pump_set_state` → `set_pump_state`, since the
  schedule has a state too). Read the table rather than transposing by hand.
  Note the rename also removes a stutter for anyone whose node is named after
  the pump: `esphome.hwr_pump_pump_set_state` becomes
  `esphome.hwr_pump_set_pump_state`. Nothing else changes — arguments,
  semantics, statuses and every field of `esphome.alpha_hwr_write_settled` are
  untouched.

  If a call is missed, the automation fails with `Action ... not found` — which
  names the service you called but not its replacement, so the table above is
  the thing to check against. Note also that Home Assistant does not validate
  service names when an automation is reloaded, only when it fires: a rarely
  triggered automation can go on looking healthy until the next time it runs.

  The schedule services were already correct (`set_schedule_entry`,
  `upload_schedule`, `clear_single_event` and the rest all matched their command
  already) and do not move; they are also the names inherited from the original
  schedule-editor services, which is why the pump family was the side that had
  to give. **`api_bridge.cpp` now registers each service *by* its command name**
  instead of spelling the name a second time, so the two surfaces can no longer
  disagree — the drift was possible only because each side named the write
  independently and nothing compared them. `set_vacation` / `clear_vacation`
  remain the deliberate exception: they compose the single-event slots rather
  than being commands of their own, so they settle as `set_single_event` /
  `clear_single_event`. That is now stated in the docs, along with the two
  services themselves, which were registered but undocumented.

  Fourteen of the fifteen command strings are consequently public API on two
  counts — as the event's `command` field and as a service name. The fifteenth,
  `set_remote_mode`, has no service and never had one: the Remote Mode switch is
  an entity-only write that still emits its settle event, so do not go looking
  for a service by that name. `test_write_operations.cpp::test_command_strings()`
  pins all fifteen regardless — including a count check, so a newly added command
  fails the test until it is pinned too, and a uniqueness check, because two
  commands sharing a string would now register two services under one name.

- **README recipes §1–§3 now declare `external_components` at `@main`**, as §4
  and §5 already did. A package fetched at `@main` self-declares its component
  source pinned to the release it shipped with, so without a top-level
  override the component stays at that tag while the package config moves ahead
  — and any key added since the release is rejected as an invalid option. This
  is not a latent risk but a present one: `data_timeout` was added to
  `alpha_hwr` after the v0.15.0 release and is documented as user-settable, so
  §1 as previously written rejects it today. Verified by execution both ways —
  invalid without the override, valid with it.

- **`write_bench.py chain`** runs several services over a single connection,
  resolving every service once up front. Each connection costs an
  `APIConnection` and its frame buffers on a node with ~72 KB free, and four
  stacked clients were enough to exhaust it (issue #127) -- a bench harness
  that reboots the node it is measuring. Note that the service-list encode in
  that crash's backtrace was the *victim*, not the cause: the failing
  allocation was at most ~48 bytes, so the heap was already gone.

- **`dhw_demand.cpp` is now compiled and driven by the host test suite.**
  `esphome compile` was the only thing in the toolchain that compiled it at
  all: 538 lines holding the sensor-callback wiring, the branch that selects
  between the pump-ON and pump-OFF detection paths, the flow latch, the
  derivative helper and every publish — none of it reachable by the unit suite,
  by cppcheck, or by the mutation check. The consequence was not theoretical:
  while closing the pump-state coverage gap, that file was left with a block
  referencing an out-of-scope variable, and 905 host assertions plus cppcheck
  all reported green. Only the firmware build objected.

  The ESPHome entity mocks were extended to the point where the component
  compiles against them, and `tests/test_dhw_demand_component.cpp` now
  instantiates the real component, wires mock entities through its real
  setters, and ticks `update()` on an injected clock — asserting on what the
  output entities received. It covers what lives in the `.cpp` and nowhere
  else, including the frozen-motor case that previously needed a bench session
  with a deliberately shortened timeout to provoke. Six mutations now point at
  `dhw_demand.cpp`, which was an unreachable file for the mutation check for as
  long as nothing host-compiled it.

  The same gap turned out to cover four more files, so they were closed too:
  `auth.cpp`, `sensor_publisher.cpp`, `telemetry_service.cpp` and
  `device_info_service.cpp` each compiled against the mocks unmodified and were
  firmware-build-only for no reason but a missing target. Each got a test suite.
  (`auth.cpp` and its suite have since been deleted outright — see the opening
  sequence entry below.) The publisher's presence gating, temperature bounds, head-
  rate derivative and issue #127 text guards; the telemetry poll set and the
  OpSpec routing that tells alarms from warnings by an echoed register; and the
  device-info string reads with the two hand-ported repairs for a pump that
  drops the first character of its product name and serial.

  Host-compiling `device_info_service.cpp` immediately found three dead things
  the firmware build had never reported — a file-static `TAG` shadowed by the
  class's own, a lambda capturing `this` without using it, and an unused
  `Session &` member, now dropped from the constructor.

  1052 host assertions, up from 905, and 31 of 31 mutations caught.

  Only `alpha_hwr.cpp`, `ble_connection_manager.cpp` and `api_bridge.cpp`
  remain firmware-build-only for real reasons (ESP-IDF and the API SDK), plus
  `time_service.cpp`, which wants a `real_time_clock.h` mock that does not
  exist yet.

  The mocks deliberately mirror ESPHome's real behaviour rather than a tidier
  version of it — `sensor` and `text_sensor` publish unconditionally while
  `binary_sensor` de-duplicates, and `has_state()` means "has published", never
  "is fresh". Both asymmetries are the reason `publish_gate.h` and the
  detector's staleness registers exist, so a friendlier mock would have made
  each of them look like an oversight.

- **The test suite now links production code instead of validating copies of
  it.** Three test files asserted hand-written replicas of shipped logic, so
  they passed regardless of what the firmware did — corrupting the CRC table and
  swapping the CRC bytes in the packet builder left `make test` reporting 21/21
  with byte-identical output. `tests/protocol.h` is now a forwarding shim onto
  `codec.cpp`/`frame_builder.cpp`; the Class 3/7 response-match predicate moved
  into `components/alpha_hwr/response_match.h` so production and the test share
  one implementation; and `ControlService`'s notification-driven state is
  asserted against the real service in `tests/test_control_service.cpp`. One
  replica remains, covering `handle_remote_mode_ack()` — remote mode has no
  `WriteCommand`, so that path has no production-linked coverage anywhere;
  routing it through the write-operation layer would close both that gap and
  the standalone-write-path violation.

- **`tools/mutation_check.sh` and a CI job guard against replica-testing
  returning.** It breaks production code on purpose, one mutation at a time, and
  fails if the suite does not notice. Seven mutations, all currently caught. It
  refuses to run against a tree with uncommitted changes to any file it mutates,
  restores from `HEAD` rather than the index, aborts if a restore fails rather
  than continuing over a source it could not put back, and treats a mutation
  whose target has moved as a failure rather than skipping it.

- **`ScheduleService::read_entries()` and `clear_entry()` are removed.** The
  synchronous read wrote through a caller-owned pointer from a callback that
  fires seconds later, which is a use-after-scope for the stack vector the
  documented recipe told users to pass. Its own default argument masked the
  hazard by failing before any wire traffic, so only the explicit-layer form —
  the one the recipe used — reached it. Use `read_schedule_entries_async()`,
  which owns its vector, and route writes through the write-operation services.

- **The pump-on subtraction no longer differences across its own pump start.**
  The two flow channels do not begin reporting together: at a start the meter
  publishes loop flow before the pump publishes its own, measured at 13 s. For
  those seconds `flow − pump_flow` was taken against a **stale zero from before
  the motor started** — inside the 30 s staleness bound and past the speed
  floor — and published `demand_level` 0.57 at confidence **0.90**, the top of
  the tier's range. Measured on the Python detector over 30 days, 42 % of all
  subtraction firings fell within 10 s of a pump-on edge, against 8.8 % of
  pump-on cells overall; scored against its bench corpus the fix removed 3 of 6
  false positives with **no true positive lost** (precision 0.903 → 0.949).
  `reading_predates_pump_start` is a regime test, not a suppression window:
  nothing is blocked for a fixed time and the tier resumes on the pump's very
  next reading.

- **The pump-on subtraction declines while the pump's flow estimate is still
  spinning up** (`pump_on_demand_settle_seconds`, new, default 10 s). The
  sibling of the fix below it, and the one that dominates in production: there
  the pump channel is silent across the start, here it reports *promptly* while
  the impeller is still accelerating, so the estimate reads low against a loop
  the meter already sees moving — `pump_flow 0.713` at 3172 rpm against a meter
  reading 1.712, published as 1.00 GPM at confidence 0.81.
  `pump_on_demand_min_speed_rpm` does not cover it: that floor is for a low
  *steady* speed. Measured on the Python detector across 296 pump starts in 30
  days, `meter - pump_flow` by age of the run: 0–10 s is p90 0.820 GPM with
  26.6 % above the 0.3 threshold, against p90 0.13–0.19 and a flat 6–9 % for
  every band out to 180 s. Over three production days the subtraction's startup
  firings fell 13 → 1 with no session lost. Set to `0` for the previous
  behaviour.

- **The falling-edge flow latch is disarmed for 30 s after a pump-off edge**
  (`latch_pump_off_suppression_seconds`, new, matched to `flow_latch_seconds`).
  The latch exists for gaps between meter reports during a draw; a shutdown
  presents the same shape because loop flow runs a median 1.45 GPM and
  collapses through the threshold within seconds of the motor parking. Since
  the flow signal outranks thermal and charge on confidence, a *published*
  thermal or charge verdict means flow was below threshold now and above it
  within the latch window — exactly what a shutdown supplies, while the thermal
  signal fires because the pump has been returning cooled loop water to the
  tank. Measured on the Python detector over 30 days, 71 % of thermal and 62 %
  of charge firings fell within 30 s of a pump-off edge against 0.38 % of
  pump-off cells; over a production week the fix removed **93 false positives
  for 1 true positive** (precision 0.768 → 0.810, recall flat at 0.993). A real
  draw puts the meter above threshold on its own and never consults the latch,
  which is why it costs almost no recall — controlled runs of a draw spanning a
  shutdown, and of a draw starting 41 s after one, are detected identically
  with it on and off. Set to `0` for the previous behaviour.

  Both defects were found and fixed on the Python detector first
  (`dhw-sensor-apps`) and are ported here to keep the two implementations in
  parity; neither had an equivalent guard on this side.

- **BREAKING — `droplet_max_stale_seconds` is now `flow_max_stale_seconds`** —
  the staleness bound on the household flow channel was named after the meter
  one installation happened to use. `dhw_demand` reads whatever GPM sensor you
  wire to `flow`, so the key now names the channel rather than a product.
  Behavior, default (60 s) and semantics are unchanged; only the spelling
  moves. A config still setting the old name **fails at `esphome config` time**
  naming the replacement, following the same rule as the keys retired in #149:
  config that validates but does nothing is a trap.

- **BREAKING — the Head sensor is now named "Head", not "Head Pressure"**
  ([#157](https://github.com/eman/esphome-alpha-hwr/issues/157)) — head is a
  length, measured in meters, and calling it a pressure was wrong everywhere
  except the entity name: Grundfos calls it "Head (H)", the config key is
  `head:`, the package header comments say "Head (m)", `docs/units-audit.md`
  says "Head", and the history trend label is "Head". The diagnostic
  `head_rate` entity follows as **"Head Rate"** (was "Head Pressure Rate").
  Only the names shipped by `packages/alpha_hwr_base.yaml` and
  `packages/alpha_hwr_pairing.yaml` change; the component sets no default name,
  so a config that writes its own `alpha_hwr:` block is unaffected.

  **This orphans the old entity in Home Assistant.** ESPHome derives the API
  key from a hash of the object_id and the Home Assistant integration builds
  `unique_id` from that key, so the renamed sensor arrives as a *new* entity —
  `sensor.<device>_head` — and `sensor.<device>_head_pressure` is left behind
  with its history and long-term statistics.

  **There is no registry-side way to carry that history onto the new entity.**
  Home Assistant does migrate statistics when an entity is renamed, but it
  refuses a rename onto an entity_id that is already taken (`Entity with this
  ID is already registered`) — and the live sensor already holds
  `sensor.<device>_head`. Deleting the orphan first does not help either: it
  frees the *old* id, not the new one, and the two entities have different
  unique_ids, so nothing links them.

  **Migration, one line — this is also the only way to keep your history:** to
  stay on the old entity_id, re-declare `name: "Head Pressure"` under `head:`
  in your own `alpha_hwr:` block. The main config wins over the package, the
  same override the `pump_head_rate_sensor` migration in 0.15.0 uses.

  If you would rather take the new name, accept that the series restarts:
  delete the orphaned entity, and clear its leftover long-term statistics under
  **Developer tools → Statistics**.

- **The Head sensor carries `device_class: distance`**
  ([#157](https://github.com/eman/esphome-alpha-hwr/issues/157)) — it had none,
  because `m` is not a valid Home Assistant `pressure` unit, so Home Assistant
  offered no way to display it in anything but meters. Meters of head is
  dimensionally a length and HA's `distance` class accepts both `m` and `ft`,
  which unlocks the per-entity unit picker — the pump's datasheet leads with
  feet (§13: "Head (H) 15-55: max. 18 ft (5.5 m)").

  **Nothing about the value changes.** The decode path, the published state and
  the recorded unit are all still meters; `distance` is display metadata and the
  conversion happens in the frontend, per user. Unlike the kPa → m change in
  0.13.0, history does not step at the upgrade.

- **Docs and comments no longer name a specific flow meter or water heater** —
  README, `AGENTS.md`, `docs/configuration.md`, the packages and the component
  comments now say "household flow meter" and "water heater". The detector was
  always sensor-agnostic; the prose implied otherwise. Example entity IDs in
  the packages changed to neutral placeholders (`sensor.dhw_flow_rate`,
  `sensor.water_heater_*`) — these are illustrative substitution values, so
  they change nothing at build time.


- `tools/mutation_check.sh` checks every entry's search string before it builds
  anything, and grew a `--verify` flag that does only that. An entry pointing at
  code that has moved is scored `(not applied)` and turns the sweep red — which
  is correct, but only after the better part of an hour, and only for the
  entries a filter happened to select. The static check answers the same
  question for all of them in about seven seconds. Retargeting the three entries issue
  #259 invalidated is what prompted it: one was noticed while writing the
  change, two were found by the sweep.

### Removed

- **Seven dead schedule methods, and the doc recipe that pointed at the worst of
  them.** `ScheduleService` kept a parallel write surface from before the
  write-operation layer existed (issue #92): `write_entries()`,
  `write_entries_async()`, `enable_schedule()`, `disable_schedule()`,
  `set_state()`, `clear_entry_async()` and `clear_single_event_async()`, plus
  the `build_schedule_apdu()` helper only the first two used. None had a caller.
  The facade passthroughs `write_schedule_entries()` and
  `write_schedule_entries_async()` go with them.

  Deleting rather than repairing, because the pair a user could actually have
  reached never sent the configuration commit the pump needs to persist a layer
  — the surviving `write_cached_layer_async()` does, which is the difference
  that matters. The async one also reported success on a timeout, though that
  part is less damning than it first looks: the surviving path reports `true`
  the same way, and gets away with it only because `WriteOperationService` sits
  above it and derives the real verdict from a read-back confirm. The deleted
  method had nothing above it. And the header steered the next developer
  straight at it: "Use `write_entries_async()` for proper transaction
  handling", naming the one method in the class with neither a transaction nor
  anything to check its result.

  The lambda recipe in `alpha_hwr_schedule.yaml` that taught both is replaced by
  the live path — `set_schedule_entry()` for one entry, the `upload_schedule`
  service for a whole grid — each of which is serialized, committed, confirmed
  against a read-back, and reported in a settle event.

  Two things worth separating from the audit note that prompted this: the
  facade's `enable_schedule()`/`disable_schedule()` are **not** affected. They
  share a name with the deleted `ScheduleService` methods but route through
  `submit_set_schedule_enabled()`, so the commented example in
  `hwr-pump-example.yaml` points at working code and stays. And the audit's
  `clear_entry()` was already removed; the `clear_entry_async()` deleted here is
  a different method that was also never called.


- **The session's ERROR state, which nothing could enter** (issue #174 audit).
  `session.h` documented six states and a `* -> ERROR : Any operation fails
  critically` transition, complete with an ASCII diagram. `Session::on_error()`
  and `Session::reset()` had no caller anywhere in the component, so that
  transition could not occur — and `is_error()`, `get_last_error()` and
  `last_error_` were dead behind it.

  Removed rather than wired up, because the state was redundant with what the
  component actually does. A failure that matters ends the BLE link, which
  arrives as `on_disconnected()` -> IDLE and is recycled by the inbound-data
  watchdog; the user-facing cause is reported by the fault-string hold, which
  carries more than a state could. The clearest evidence it was never a distinct
  state is that `is_connected()`, the only predicate that ever inspected it,
  treated ERROR exactly as IDLE.

### Fixed

- **The receiver accepted a frame start the pump never sends, and bounded the
  length field wrongly at both ends** (issue #278). All latent — the largest
  frame in the corpus is 61 bytes and the reported fragment was caught by the
  CRC either way — but each is a way into the phantom-reassembly path issue #259
  is about.

  `is_frame_start()` accepted `0x27` as well as `0x24`, justified by a comment
  calling it "Request frame (client → pump, also echoed back)". The captures
  refute the echo: across 44,200 CRC-valid frames, all 22,138 phone→pump frames
  begin `0x27` and all 22,062 pump→phone frames begin `0x24`, with none of the
  latter beginning `0x27`. `on_notification()` is fed GATT notifications only,
  so it never sees our own writes. The claim turned out to be *inherited* rather
  than derived — the Python client carried the same test with the same wording —
  and it was written down in five places here, not one.

  A declared length below the floor no longer starts a frame. **The floor is 4,
  not 5.** 5 is the corpus minimum, but no frame in that corpus is refused at
  the APDU head, and the zero-payload shape occurs only in such a refusal — so a
  floor taken from the corpus rejects the 8-byte `Unknown Class` this project
  handles deliberately. That mistake was made and caught by the suite.

  The ceiling moved from 256 to **259**, the largest telegram the specification
  permits. It was briefly set to 257 here, from reading the length field as
  bounded by `MAX_PDU_LEN` alone; the specification puts DA and SA outside the
  PDU bracket, so the field counts `2 + PDU ≤ 255` and the telegram is `≤ 259` —
  and the Grundfos GO app's own builder rejects a length field above 255, not
  above 253. The 257 reading also contradicted this change's own floor, which is
  derived the other way.

  Two things follow from that, and both are worth stating because they make the
  ceiling change smaller than it looks. What actually protects a legal frame is
  not the ceiling but the completion test: a frame at its declared length has
  already satisfied it and skips the overflow guard whatever the cap says. The
  constant is corrected for what it *says* — a buffer bound naming 256 when a
  GENI packet can be 259 is wrong documentation — rather than for what it does.

  And the reassembly-overflow branch **can no longer fire**. The expected length is
  `data[1] + 4`, so at most 259, and the cap is now that same 259 — a buffer
  above the cap is therefore also at or past the expected length, which is the
  completion test. The branch was reachable only because the cap sat three bytes
  under a legal frame. It stays as a backstop, documented as unreachable, but
  the three tests and three mutation entries that exercised it have been re-aimed
  at the CRC-drop path, which is a route into the same state that actually
  exists. Unbounded growth is bounded by the completion test and by
  `REASSEMBLY_TIMEOUT_MS` regardless.

  Two further defects in the same function, both found by an adversarial pass
  and both reproduced. A frame start delivered as a **one-byte notification**
  armed reassembly before its length byte existed, and nothing recomputed the
  expected length afterwards — so the completion test could never fire and every
  notification that followed was swallowed for a second, up to ~18 replies at
  the corpus's median latency. And the overflow guard ran ahead of the
  completion test while testing the accumulated size rather than the declared
  one, so a **complete, CRC-valid frame delivered with trailing bytes** was
  discarded for the sake of bytes that were never part of it — the exact case
  the completion test's `>=` and the trim below it exist to handle.

- **A read in flight when the link dropped was never told, and one corrupt
  inbound fragment could do the same thing to a live link** (issue #259).
  `Transport::reset()` cleared the command queue without invoking the queued
  commands' callbacks. A service waiting on a reply therefore heard nothing ever
  again — no reply, no failure, and no timeout either, because the timeout lived
  in the queue entry that had just been discarded.

  Every consumer that survives this today survives it because somebody wired it
  a *separate* hook for the same event: `WriteOperationService::on_disconnect()`
  (issue #92), the read-chain generation counter (issue #18),
  `ControlService::invalidate_cache()`. That is three hand-written compensations
  for one missing contract, and a fourth caller — the next `*_async` read anyone
  adds — gets none of them. The opening sequence used to carry a whole-sequence
  backstop for exactly this and it went away with the sequence (issue #229),
  which is what the reporter noticed.

  `reset()` now fails what it abandons: each queued callback is invoked with
  `(false, nullptr, 0)`, the same verdict a timeout delivers, so a multi-command
  read unwinds through the failure branch it already has and reaches its
  caller's `on_complete`. A chain that continues past a failed step sends its
  next read from inside that callback, so those are taken into the same drain —
  otherwise the unwind stops half-done and the hang has only moved one command
  along. The drain is a loop rather than a recursion because the longest chain
  in the tree is as long as the pump says it is (`EventLogService` reads
  `min(available_entries, max_entries)`, both straight off the wire), and it is
  capped so that a chain re-sending on every failure cannot spin the task
  watchdog into a panic.

  The second half is the live-link path the reporter supplied evidence for. When
  the reassembly buffer overflows, `on_notification()` used to call `reset()` —
  so one corrupt fragment declaring a long frame cancelled every read in flight,
  with nothing telling any caller, on a link that was still up. It now drops the
  partial frame and nothing else. Losing track of where a frame begins says
  nothing about whether the pump will answer the commands we already sent; if
  the frame that overflowed *was* someone's reply, that command's own timeout
  reports it, through the path every caller already handles. The peer-resync
  hold and the reply debt now survive that path on their own, instead of being
  saved and restored by hand around a call that should not have been there.

  One consequence needed handling on the way. Because an abandoned chain now
  reaches its terminal branch, and that branch is where the trend and event-log
  display caches are written, a dropped link would have replaced a good display
  with however many entries happened to land first — indistinguishable, at that
  point, from a genuinely short log. Both services already open with a
  readiness check; they now apply the same check at the far end, so a read cut
  short keeps the previous data instead of publishing a truncated one.

  An adversarial review pass found three more consequences, all now fixed and
  pinned. `Pump Clock Drift` published NAN on every disconnect that landed while
  its read was queued — the one leg of the initial read chain that captured the
  component but not the read-chain generation, so it was the only consumer the
  new callbacks reached uncompensated. The same move-then-pop discipline the
  failure paths got was missing from the three *success* completions, where the
  consequence is worse (a `pop_front()` on a deque the callback emptied); a
  callback does not become safe by having succeeded. And the drain's re-entrancy
  guard was documented as an equivalent mutant on the strength of an experiment
  that could not reach it — a chain that queues its next read *before* resetting
  recurses one drain per step, and the cap is counted per call, so it is no help
  at all.

  It also retires a guard added only two changes ago. `ControlService`'s
  setpoint-range read carries an in-flight flag, and issue #273 had
  `invalidate_cache()` release it because a disconnect mid-chain would otherwise
  leave it set for the life of the node — silently, and with every setpoint
  write back on the fallback constants. That was a symptom of this bug, so the
  flag is now released by the chain's own callback on every path. The line stays
  (it is one assignment, and the failure it guards is silent and permanent) but
  the mutation that proved it is retired as an equivalent mutant, verified in
  two steps: the suite passes with the line deleted, and fails again with the
  line deleted *and* `reset()` put back to clearing its queue.

  A command being failed is now taken off the queue *before* its callback runs,
  which the old order got away with only because `reset()` did not invoke
  anything. `cmd` in `Transport::loop()` is a reference into the deque, and a
  callback is service code: it queues the next read of a chain, and it can reach
  `reset()`. With the new contract a callback that resets would have found its
  own entry still at the head of the queue and been invoked a second time from
  inside itself — and the `pop_front()` that used to follow it was already
  running on a deque a callback could have emptied.

- **Setpoint validation used hardcoded ranges; the pump publishes its own, per
  mode, and they are much narrower** (issue #273). `run_set_setpoint_` bounded a
  requested setpoint against constants inherited from the legacy setters. They
  are wrong in *both* directions on every mode this pump has:

  | mode | obj/sub | pump min – max | the constants |
  | --- | --- | --- | --- |
  | constant speed | 86/13 | **1650 – 3671** RPM | 500 – 4500 |
  | constant pressure | 86/15 | **1.000 – 2.450** m | 0.5 – 10.0 |
  | proportional pressure | 86/17 | **2.599 – 4.569** m | 0.5 – 10.0 |
  | constant flow | 86/39 | **0.114 – 2.498** m³/h | 0.1 – 10.0 |

  The pump keeps these in the type-301 `ControlModeFactoryConfiguration` object
  for each mode, as `min_set_point` and `max_set_point`, and the Grundfos GO app
  reads exactly those two fields to bound its setpoint slider. The component now
  reads them too, converts them to display units and validates against them.

  What this changes for a client: asking for 1200 RPM used to be accepted, sent,
  clamped by the pump to 1650, and settled `clamped` a round trip later. It is
  now `invalid` immediately, with the pump's own floor in the detail. Asking for
  10 m³/h used to be accepted against a ceiling four times the real one.

  **Proportional pressure is the worst of the four, and its values had never
  been seen.** The object is documented — the profile has 86/17 as type 301, and
  `widget_configuration_52_7.xml` binds its min and max — but the HWR setpoint
  widget does not bind it, no capture contains a read of it, and the pump never
  enters the mode in any recorded session. So the range was read off the bench
  directly. Its floor is *five times* the constant we were using, and its range
  does not overlap constant pressure's, though the constants treat the two
  identically.

  The constants stay as the fallback rather than being deleted, and the fallback
  is deliberately the wider range: with nothing read from the pump the honest
  position is that we do not know the limit, and refusing a setpoint the pump
  would have taken is worse than letting the pump clamp it — the readback
  reports what it stored either way. The settle detail says which bound refused,
  because "the hardware cannot" and "we have not looked" are different facts.

  `resulting_min_set_point`, the fourth float in the struct, is deliberately not
  read. It is not a floor: on the bench pump it is −3671.0 for constant speed,
  and 9804.0 — constant pressure's minimum — under proportional pressure, whose
  real floor is 25490. The app ignores it too.

  The four reads run **after** the cache-sync verdict rather than before it, so
  time-to-ready is unchanged; they gate nothing, and a pump that will not answer
  leaves each mode on its fallback.

  Note what the range is: the mode's **factory** range, which does not account
  for an active limiter. The pump also has MaxFlow and MinFlow limiters
  (86/600–659), off by default but settable from the Grundfos app, and one that
  is enabled holds flow below the setpoint without changing any of these numbers
  — issue #274 covers reading them.

  A degenerate answer (max at or below min) is
  refused as a source rather than cached, so `setpoint_ranges_known()` cannot
  claim a complete set off the back of one — though `get_setpoint_range()`
  re-checks the invariant on every call, so a cached one would not actually have
  bounded anything. The ranges are dropped on disconnect, because the next
  connection may be a different pump.

  Two properties of the read are load-bearing rather than tidy. The chain **stops
  at the first failure**: all four objects are type 301 version 1, so all four
  reads declare the same expectation and the transport — which matches on object
  type and never on the instance — cannot tell their replies apart. A chain that
  carried on would hand a timed-out read's late reply to the next mode, shifting
  every remaining range by one slot; constant pressure would end up bounded by
  constant speed's 1650–3671 read as Pascals, refusing an ordinary 1.5 m setpoint
  as `invalid` and blaming the pump for the rest of the connection. And a second
  chain cannot start while one is in flight, which would otherwise put eight
  reads on the wire and let the older one publish the completeness flag — with
  that guard released on disconnect as well as on completion, since
  `Transport::reset()` drops a queued command without invoking its callback, so
  a drop mid-chain would otherwise leave it latched and the ranges never read
  again for the life of the node.

  Seven host tests, the load-bearing one being a simulated pump whose range is
  *wider* than the constants — accepting 5000 RPM is possible only by using the
  pump's number, where a narrower range would also pass against code that had
  merely become stricter. Six mutation entries. The simulator answers with a
  byte-faithful type-301 frame carrying, for proportional pressure, the
  genuinely deceptive `resulting_min_set_point` the real pump sends there:
  9804.0, which is *constant pressure's* minimum — a plausible positive number,
  in the right unit, for the wrong mode.

  One thing this changes for the Home Assistant sliders, which reach the same
  validator: their declared bounds are now wider than the pump on every mode, so
  part of each slider's travel is a refusal rather than a value the pump clamps.
  Narrowing them means re-sending entity info against Home Assistant's cached
  registry, and is tracked separately.


- **The "dedicated" setpoint write was addressed backwards, and the pump had
  been refusing it since the day it was written** (issue #258). It is gone
  rather than corrected, and the reasoning is worth recording because the
  obvious repair would have been worse than the defect.

  `ControlService::set_class10_setpoint()` laid its address out sub-id first,
  `[SubH][SubL][ObjH][ObjL]`. Every Class 10 SET this pump accepts is object
  first, `[Obj][SubH][SubL]` — all twenty distinct address shapes across the 420
  SETs in the captures, and all nine shapes this component sends. For the speed
  setpoint the frame therefore said object `0x00`, and the pump answered
  `Unknown Data Item`, quoting our own first payload byte back at us. Bench-seen
  during #256's verification, which is what made these replies visible at all;
  before that the send was fire-and-forget and nothing ever read the answer.

  Correcting the byte order would have produced a *worse* frame. Sub-ids 13, 15
  and 39 are not setpoint registers: the GENI profile shipped inside the
  Grundfos Home APK names them
  `control_mode_cs/cp/cf_factory_config_obj`, object type 301
  `ControlModeFactoryConfiguration` — a 28-byte struct of seven floats beginning
  with `default_set_point`. The per-mode *user* setpoints are sub-ids 14, 16 and
  40, type 302, 18 bytes including three PID terms. So the write would have put
  four bytes of float into the factory defaults, with no type, version or size
  header, and no payload for the other 24 bytes.

  Correcting the byte order would not have produced a *legal* frame either, which
  is the part worth keeping. Sub-ids 13, 15 and 39 are object type 301
  `ControlModeFactoryConfiguration` — `ReadWrite`, but a 28-byte **struct** of
  seven floats beginning with `default_set_point`. A Class 10 SET to a typed
  object carries `[Obj][SubH][SubL][TypeH][TypeL][Ver][Size(3)]` and the whole
  body; every SET shape in the captures declares `9 + fixedSize`. This one
  carried a bare float, so with the address corrected the pump would read the top
  half of that float as the type word and refuse it again — on type and length
  rather than on address. Making it legal would mean a read-modify-write of seven
  floats into the *factory* record, when the live per-mode setpoint is
  `local_set_point` in the type-302 user record at sub 14/16/40. The app reads
  13/15/39 about 450 times each and never writes them.

  Nothing needed it, and the **bench** settles that rather than the captures.
  With the write deleted, `set_setpoint constant_speed 1900` sends the fused
  request and its commit and nothing else, and the pump's stored setpoint moves
  1800 → 1900 with the readback confirming. The operation's verdict has always
  come from that readback, so the refusal changed no outcome — which is exactly
  why it survived.

  What the captures do *not* show, and an earlier draft of this entry wrongly
  claimed: the app never uses object 86 sub-id 6 to **change** a setpoint. All 25
  of its sub-6 writes are start/stop presses passing the mode's current value
  through — 12 of them carry the map default 3671.0 — and `constant_flow.log`
  changes a setpoint with no sub-6 write at all, using the type-302 user config
  at sub 40 (2.000 then 1.500 m³/h). The app's own widget configuration agrees:
  it binds sub-6's `set_point` as the *displayed* current value and binds the
  three per-mode editors to `control_mode_{cs,cf,cp}_user_config_obj.local_set_point`.
  Whether this component should follow it there is filed separately; what is
  settled is that the fused write works on this pump and the deleted one never
  did.

  What the deleted write *did* do was schedule the configuration commit, and that
  commit is real: every one of the 25 fused writes is followed immediately by an
  object 84 sub-id 1 overview write, with nothing between them but the short
  acknowledgement. `send_control_request()` issues it now via its own
  `queue_commit`, so a setpoint write still commits, 200 ms after the frame
  instead of 600 ms. The confirm readback lands where it always did — and it is
  pinned now: `SETPOINT_STEP2_DELAY_MS` named a step that no longer exists, so it
  and `SETPOINT_CONFIRM_DELAY_MS` fold into the one number that was always the
  real one, 1600 ms. The simulated pump gained an apply latency, because without
  one a confirm at 1600 ms, at 1200 or at 0 all passed and the whole #82/#85
  settle rationale survived only as a comment.

  The mislabelled comments that made this easy to write are corrected too.
  `send_control_request()` and `send_set_mode_request()` both annotated their
  address bytes `Sub ID high` / `Obj ID high`, which is backwards; they were
  right only by coincidence, because object 86 is `0x56` and a sub-id under 256
  fits the low byte either way round. The deleted write took those labels at
  their word. The `Sub 0x5600 / Obj 0x0601` spelling used throughout the file is
  named for what it is — two-byte slices taken at the wrong boundary, entrenched
  nicknames rather than addresses — so nothing is inferred from it again.

  **The simulator now refuses what the pump refuses.** It used to accept an
  OpSpec `0x88` SET at any address, which is why the host suite showed this write
  working for as long as it did. It answers an unrecognised Class 10 SET with
  `Unknown Data Item` and the offending item id, exactly as the pump does, and
  the suite fails if any test provoked one — a net over every Class 10 SET the
  component sends, not just this one. It checks the declared length against the
  object's body size too, so the *repaired* frame is caught as well as the
  original: a bare float at a real address clears both the old address check and
  the existing declared-vs-carried check, and the pump would still refuse it.
  Mutation entries reverse the fused request's address and shorten the confirm
  delay, proving both nets catch.

  **New tool: `tools/geni_capture_scan.py`.** The capture corpus has a trap its
  README has documented since #248 — almost every write exceeds the 20-byte ATT
  payload and is split across several, so a scanner reading packets individually
  finds every read and 13 of the 420 writes — and the project has fallen into it
  twice. The scanner reassembles ATT value streams before scanning, drops the
  duplicate sessions, and reports SET/GET/reply/latency censuses; reassembly is
  self-checking at exactly 100% coverage of every GENIbus stream with nothing
  left over. Which layer matters is worth naming: the app sends a sequence of
  independent ATT Write Commands rather than one long PDU the controller
  fragments, so it is the ATT-value concatenation that recovers a write, not the
  HCI continuation flag. It carries a `selftest` — CRC against a captured frame,
  frame-walker edge cases, PacketLogger endianness both ways, and the census
  itself when the corpus is present — wired into the Python CI job, where the
  corpus half skips because `resources/` is gitignored. Several counts in the
  protocol comments were
  pre-reassembly artifacts and are corrected against it: **420** Class 10 SETs
  (was 195), **459** short replies split 420 OK / 26 BUSY / 13 OPERATION_FAILED
  (was 136 / 100 / 24 / 12), 25 fused control-request writes (was 16), 40 layer
  writes (was 20), 31 mode writes at 38–113 ms (was 12 at 38–85), 77 overview
  commits at 36–193 ms (was 34 at 50–193), and p99 latency 144 ms over 19,768
  pairs (was 121 over ~12k). None of the conclusions those numbers supported
  changes.

  On 420 specifically: it has been published, retracted as "not reproducible at
  any level of de-duplication", replaced by 195, and is now back. The retraction
  gave a checkable reason — that a request telegram can carry more than one APDU
  and counting per APDU yields 195 — and it is false twice over. All 44,200
  telegrams in the corpus carry exactly one APDU, so the two counting rules are
  the same rule here; and counting per APDU could not produce a *smaller* number
  in any case. 195 is what a scan produces when it loses frames. `selftest` pins
  the census now, so a fourth figure cannot appear quietly.


- **A single event scheduled far ahead evicted a live nearer one** (issue #262).
  Bench-observed, not inferred: two writes fifteen seconds apart on a pump with
  five empty slots, one for tomorrow and one for 2040. Both settled `accepted`,
  both took slot 0, and the readback showed only the 2040 event. The nearer one
  was destroyed with four slots free, and nothing anywhere said so.

  The auto-slot resolver decides which slots are recyclable by asking which
  stored events have expired, and it asked that against **the new event's own
  begin timestamp** instead of against the clock. For an event a few minutes out
  the two questions have the same answer, which is what the Lovelace card's Quick
  Run *presets* produce. For an event years out they do not: a 2040 event makes
  everything in the next thirteen-odd years look expired, so the picker returned
  a slot holding a live event and the write overwrote it. `set_vacation` resolves
  through the same line, and a vacation is months out by nature — one booked for
  next summer would have cleared every single event before it.

  Under-reached before now, not unreachable. Every path through the Home
  Assistant services was blocked at the parser until #255, so no service call
  arrived. Two surfaces were never behind that parser, though: the schedule
  editor's **Set Vacation** button calls `submit_set_vacation()` on the component
  directly, and `build_event_window()` anchors it to the current calendar year,
  so it could place a vacation up to eleven months out — far enough to expire
  every live event in the pool. The card's **Custom Run** date pickers take an
  arbitrary date too, but they go through the service, so #255 held them off.

  Fixed by measuring expiry against the node's wall clock. The reference
  timestamp is no longer optional — a caller with no clock has to say so, by
  passing 0, and 0 means **expire nothing** rather than expire everything: a
  picker that cannot tell the time refuses to recycle rather than guessing which
  events are over. The refusal names the clock (`"no free single event slots
  (node clock not set, so expired events cannot be reused)"`) so a node that has
  simply never synced does not read as a pump with a full slot pool.

  The eviction was also silent by construction, which is most of what made it
  expensive to diagnose: the operation settles `accepted` because it did write
  successfully, to a slot it was entitled to choose, and nothing compared the
  slot's previous contents against what replaced them. Recycling a slot now logs
  a WARN and carries the same sentence into the settle event's `detail`, naming
  the slot and the window it replaced.

  Two more things the picker was getting wrong, both found by the adversarial
  review rather than by the bug report:

  - **An empty slot now always beats a recyclable one.** The loop took the first
    index no *live* event held, so an expired slot 0 went ahead of four empty
    ones every time. On a five-slot pump repeated one-time runs cycled through
    slot 0 forever while slots 1–4 stayed empty — destroying a stored record on
    every write, and firing the new "this slot was recycled" warning on writes
    that cost nothing, which is how a warning stops meaning anything.
  - **When there is nothing empty, the stalest record goes.** Recycling by
    lowest index kept the oldest event and threw away the most recently
    finished one.

  The **Add Single Event** editor button no longer picks a slot at all. It used
  to call the picker and then write to the returned index, with nothing closing
  the gap between the two: a service call resolving in that gap takes the same
  slot, writes a live event to it, and the button's write overwrites it. It now
  submits with no slot and lets the write-operation layer resolve one at the
  moment it writes — which is what AGENTS §6 asks for anyway, and which also
  gives that button the recycling warning and the settle detail that a write by
  index skips. The free-slot accessor it used is gone with it, so the
  pick-then-write shape is no longer reachable from a lambda. "No free single
  event slots" now arrives as a `rejected` settle event rather than a log line.

  Seven new host tests — the reported case (a 2040 event and a live one
  tomorrow, five slots, the live one must survive), the vacation variant, both
  directions of the no-clock rule, the empty-slot preference, a pool genuinely
  full of live events, and the picker's decision table called directly at
  hand-chosen reference times, including the boundary where an event ends
  exactly *at* the reference (it keeps its slot; one second later it does not).
  The existing reuse test is retargeted at the clock and now asserts the recycle
  note. The single-event fixtures anchor their windows to the node clock, which
  is not cosmetic: `test_single_event_auto_slot`'s "live" event ended in 1970,
  so it was live only relative to the new event's begin, and its slot assertion
  held under the old comparison and failed under the fixed one. Seven mutation
  entries, including the reported line itself, all verified caught.

  Verified on the bench: the issue's own two writes, on a pump with five empty
  slots, land in slots 0 and 1 with both events surviving, and a vacation booked
  for July 2027 takes slot 2 and evicts neither. Recycling a slot that really
  had ended reports `reused slot N, which held an event that ended (…)` in the
  settle event; a write into an empty slot reports nothing.

  Four follow-ups came out of the review, all pre-existing: #267 (a finished
  vacation is still "the" vacation, so `clear_vacation` can clear the wrong
  slot), #268 (the write-op suite pins `TZ=UTC`, so the cache's UTC invariant is
  invisible to it), #269 (a wholly-past event is written and settles accepted),
  #270 (four independent answers to "what time is it", with three sanity floors).


- **`set_single_event` and `set_vacation` rejected every input on real
  hardware** (issue #255). Both services answered a terminal `invalid` to any
  argument a client could send, with `seq: 0` — the request never reached the
  write layer. On the device there was no input either service would accept, and
  there had not been for as long as the parser has existed.

  The bound that says "fits the `uint32` the wire carries" was passed as a plain
  `long`. `long` is 32 bits on the ESP32-C3 (RISC-V, ILP32), so
  `parse_int_field(s, 0, 4294967295L, &v)` compiled to `hi = -1` and the guard
  became `if (v > -1) return false` — a rejection of every value at or above
  zero. The literal itself was fine; the damage was at the call, where it
  narrowed into the parameter.

  What this broke, on a pump: the Lovelace card's **Quick Run** and **Custom
  Run** buttons, which call `set_single_event`, and any automation or script
  calling either service. The vacation date-pickers in
  `packages/alpha_hwr_schedule_editor.yaml` are unaffected — they call the
  component directly from a lambda and never pass through this parser — as are
  `clear_single_event`, whose 0–99 bound fits a 32-bit `long`, and
  `clear_vacation`, which takes no data argument and parses nothing.

  Fixed by naming the parse width once and fixing it: every bound and every
  parsed value in the bridge now travels as `long long` via a `ParseInt` alias,
  and parsing goes through `std::strtoll`. That second half matters on its own —
  `std::strtol` on a 32-bit `long` saturates at 2147483647 and reports `ERANGE`,
  which this parser treats as a rejection, so correcting the bound alone would
  have left **every timestamp after 2038-01-19 refused** while the pump holds
  instants until 2106. Grundfos' own GENI profile for this pump, shipped inside
  the Grundfos Home app, is what settles the ceiling: `ClockProgramSingleEvent`,
  object type 220, fixed size 10, declares `begin` and `end` as `uint32_t`.

  The suite could not see any of this, and the reason is the interesting part:
  `long` is 64 bits on every host anyone runs these tests on, so the firmware
  refused inputs this file listed **by name** as accepted — `0,4294967295` among
  them — and both compiler legs stayed green. The bug was not uncovered. It was
  covered by an assertion that could not fail.

  So the suite now runs at the target's word size instead of arguing around it.
  A new CI job, **Unit tests (32-bit long)**, rebuilds `test_api_bridge` with
  `-m32` on `gcc-multilib` — same file, same assertions, `long` at 32 bits. It
  needs no new test cases to catch #255: the `0,4294967295` case that was
  already there fails against the code that shipped the bug. It also catches any
  future 32-bit narrowing anywhere in that file, which a check aimed at one
  constant cannot.

  Two `static_assert`s back it up at compile time. One reduces to
  `2147483647 >= 4294967295` and fails only on an ILP32 target, reproducing the
  defect exactly. The other ties the bound type to the parse — `long` and
  `long long` are distinct types even where both are 64 bits wide, so narrowing
  *either* the alias or `strtoll` back to `strtol` fails on every platform. That
  second one matters more than it looks: the `strtol` half is a separate
  regression with the same symptom, and asserting only the alias would have left
  it bare.

  An earlier version of that assert is worth recording, because it verified
  clean and was wrong. It allowed any bound type spelled `long long` **or**
  `int64_t`, reasoning that both are of guaranteed width. Probed on
  `ubuntu-latest`, which is what CI runs the unit tests on:

  ```
  int64_t is long:                            1
  OLD assert with ParseInt=long would PASS:   1
  ```

  `int64_t` *is* `long` there, so the very type the assert existed to reject
  satisfied it. It fired only on hosts where `int64_t` is `long long` — the
  author's machine is one of those, which is why it looked verified. A check
  that depends on which spelling a platform picked for a typedef is not a
  check, and one verified on a single platform is not verified.

  Verified on the pump. `set_single_event` with a near-future window settles
  `accepted` at `seq: 2` where it previously answered `invalid` at `seq: 0`; a
  2040 window settles `accepted` and reads back from the pump intact as
  `2040-06-01 03:00 - 03:05 (run)`, so nothing downstream of the parser narrows
  it either; and `set_vacation` settles `accepted` with `event_type: stop`. The
  rejections still reject on the device — negative, over-width, reversed and
  truncated pairs all settle `invalid` at `seq: 0`, for both services.

- **The Lovelace card mangled — and could destroy — a schedule window that
  crosses midnight** (issue #174). A cell whose end is earlier than its start
  (22:00–02:00 is stored as `[1320, 120]`) reaches the card today from the
  Grundfos GO app or the `set_schedule_entry` service, so this is a display and
  editing bug for data the card did not create.

  Three failures, worst first:

  - **Dragging such a block silently rewrote it.** The drag clamps force
    `start < end`, so grabbing an edge un-crossed the window and queued that as
    a pending write — a four-hour overnight window became a short evening one,
    with nothing to indicate the schedule had changed. Crossing blocks now
    render without drag handles, and the drag handler refuses them outright.
  - **It painted as a 4 px sliver.** The width came out negative
    (`(120-1320)/1440`) and was clamped to a 0.7% minimum, so a four-hour window
    showed as a stub at the 22:00 mark while its tooltip read "22:00 – 02:00".
    Such a block is now split into the two segments that can actually be drawn,
    and the tooltip says it crosses midnight.
  - **The editor refused to create one, silently.** `end <= start` returned
    without committing and without closing the dialog or explaining. Only a
    zero-length window is refused now, which matches what the device and both
    write paths accept.

  Both pieces are drawn on the cell's own row rather than bleeding the tail onto
  the next day: the row shows what the cell contains, and which calendar day the
  pump runs the tail on is unverified.
- **`upload_schedule` refused any window that crosses midnight** (issue #174
  audit). `parse_upload_payload()` rejected `begin >= end`, which meant the bulk
  path could not round-trip a grid the rest of the system can already produce:
  `set_schedule_entry` accepts such a window (its only ordering rule, via
  `ScheduleService::validate_entries`, is `begin != end`) and the Grundfos GO
  app can create one. Reading that grid back and uploading it was refused — a
  user could not bulk-restore their own schedule.

  Upload is now exactly congruent with the single-entry path: both reject only
  the zero-length window. The hash byte layout is unchanged.

  **What is not established:** what the pump *does* with such a window at
  runtime. It stores and echoes one back verbatim (bench-verified: 22:00–02:00
  written to an empty cell, read back as `[1320,120]`), but a byte round trip is
  not behaviour — this project has already been burned by a value that
  round-tripped byte-identically while being wrong. Whether the pump runs
  22:00–02:00, only 22:00–24:00, only 00:00–02:00, or nothing is unobserved.
  Settling it means watching motor RPM across a midnight boundary.

  **Known limitation, not fixed here:** the Lovelace card cannot display or edit
  these windows, and this predates the change — a crossing cell created by the
  GO app or `set_schedule_entry` already reaches it. It renders a negative width
  clamped to a 4 px sliver, its editor silently refuses to create one, and
  dragging such a block silently un-crosses it, which rewrites the user's
  overnight window. Tracked separately.

  Also relaxed the same rule in `tools/write_bench.py`, which mirrored it. Note
  the cost: transposing start and end (`8,0,7,0` meaning 07:00–08:00) now writes
  a 23-hour window instead of erroring. The single-entry path always had that
  hole; the two are now consistent rather than one guarding what the other does
  not.

- **A temperature-range write could overwrite the pump's own on/off-time limits
  with fabricated values** (issue #174 audit). The config write echoes those
  five bytes back verbatim so setting a temperature does not zero them (issue
  #106) — but they exist only once an Obj 91 Sub 430 read has landed. Until
  then the cache holds `ControlService`'s historical constants, which are not
  the pump's limits.

  `invalidate_cache()` clears that flag on every disconnect, and the Home
  Assistant service path reaches the write **without** `check_ready()` — the
  entity path is gated, `api_bridge.cpp` is not. So an
  `esphome.<node>.set_temperature_range` call during the initial read chain, in
  a reconnect window, or after a Sub 430 read that timed out, would send the
  constants as the pump's own limits, silently, as a side effect of setting a
  temperature.

  `temp_limits_tail_valid_` already tracked this and **nothing consulted it**.
  The write is now refused, with a detail saying why, and refused *before* the
  mode change — so it cannot leave the pump switched into temperature-range
  mode while reporting that nothing happened.

  Known limit, recorded rather than fixed: the reply's declared size is
  ignored, so a pump whose type-1012 struct is shorter inside a full-length
  frame would have padding captured as its limits and this check would not
  notice.

- **Two Class 10 writes declared a payload length they did not carry**
  (issue #174). APDU byte 1 is `0booLLLLLL` — operation in the top two bits,
  payload byte count in the low six — and two frames got the count wrong. The
  single-event schedule write sent `0xB3`, which declares 51 payload bytes,
  in a 21-byte APDU carrying 19; it had borrowed the value from the layer
  write, whose 53-byte APDU really does carry 51. The Class 10 setpoint write
  sent `0x84`, declaring 4, counting only its float value and not the four
  object/sub ID bytes in front of it.

  Neither was a visible failure, and both are now bench-verified as such: the
  pump accepts the wrong length and the right one alike, so single-event
  writes and setpoint writes have been working throughout. What was wrong was
  the frames, not the behaviour — and a firmware or pump generation that did
  check the field would have broken with no diagnostic.

  Corrected to `0x93` (SET + 19) and `0x88` (SET + 8), both bench-verified
  against a real pump: the single event lands and reads back, and the setpoint
  write switches mode and reads back the commanded value.

  The regression test is a general invariant rather than two constants: the
  write-operation suite now checks *every* APDU any test sends against its own
  declared length, so a future frame with the same defect fails without anyone
  having to think of it. It found the setpoint write immediately, which was not
  the frame it was written for. The documentation that made this easy to get
  wrong is corrected too — `schedule_service.h` described these as "OpSpec 4"
  and "OpSpec 5" with "Length varies", which is a three-bit reading of a
  two-bit field, and reading `0xB3` as an opcode rather than as SET-plus-51 is
  exactly how the length came to be copied.

- **The connection no longer opens with an "authentication handshake", because
  there was never one** (issue #174). Every connect used to send ten packets
  before anything else happened. They are gone, and nothing replaces them: the
  session is declared ready two seconds after notifications are enabled, which
  is the same delay that was always there, and the initial read chain follows.

  The whole arc, because the intermediate steps explain why removal is the
  right end point rather than a leap:

  **They were documented as unlock writes and are reads.** The second APDU byte
  is `0booLLLLLL` — operation in the top two bits, payload length in the low six
  — so the `0x03` read as "SET" is a GET with a 3-byte payload, and "register
  0x9495, unlock code 0x96" was a misparse of a length field. Decoded, the four
  are: a Class 2 GET of `unit_family`/`unit_type`/`unit_version` (answered
  `52 / 7 / 2`, an ALPHA HWR identifying itself), a Class 10 GET of the
  operation-status object (Obj 86 Sub 6, documented with object and sub
  reversed), and two INFO queries asking for scaling metadata on Class 5 item
  `0x4B` and Class 11 item `0x0F`, both answered "unscaled". Two GETs and two
  INFOs. Reads cannot change device state, so an unlock was never a thing these
  bytes could do. The same misreading had spread to `frame_builder.cpp` and
  `time_service.cpp` and is corrected there too; the address pair described as
  one 16-bit "Service ID" is a destination and a source, which the pump's
  replies show by swapping them.

  **Then they were made reply-driven, and that made the redundancy legible.**
  Classes 2, 5 and 11 joined the transport's wildcard-matched set, stages 1 and
  3 became ordinary matched reads, and completion reported how many were
  answered. What that surfaced is that every reply was discarded: the callbacks
  took `(bool, const uint8_t *, size_t)` and named none of the payload
  parameters. The component was running an interrogation sequence and throwing
  the answers away — reasonable for a general client that must ask what it is
  talking to, pointless for one that only ever talks to an ALPHA HWR.

  **And the field evidence closed the last gap.** The remaining hypothesis was
  that the packets mattered on a *first pairing*, with the pump holding state
  keyed to the bond — which would make every no-handshake observation merely a
  description of an already-paired pump. Ten connection cycles on a build with
  the sequence removed settle it: two bond-cleared re-pairings, five pump power
  cycles and three BLE reconnects, all reading five Class 7 device-info strings
  and reaching Pump Ready, nine of them accepting a Class 3 START and STOP with
  the motor confirmed spinning, across 1,019 frames containing zero Class 2,
  zero Class 5 and zero Class 11. Reported by jfriend00, who did the decode and
  ran the experiment.

  What goes with the packets: the reply-timeout gating, the whole-sequence
  backstop, the stall warnings, the fail-open ceiling and the answered-read
  counter — 758 lines of `auth.h`/`auth.cpp`, all of it machinery for keeping a
  sequence from hanging a connection it could only hang by existing. The
  session's pre-ready state is renamed `AUTHENTICATING` → `STABILIZING`, since
  nothing is on the wire during it.

  The watchdog sizing note in `link_watchdog.h` is re-derived: worst case from
  connection-open to first inbound data drops from 20.45 s to 16.0 s against the
  60 s default, and the 31.5 s backstop case disappears entirely. The
  `link_gap_report.py` floor of 41 s is deliberately *not* lowered to match —
  that is a recommendation, and changing it wants the measurement rather than a
  recomputed constant.

  Two claims are corrected rather than defended. `device_info.md`'s "Class 7
  reading requires the device to be Authenticated" is contradicted by fifty
  string reads across ten unauthenticated connections, and by the code: nothing
  in the read path checks session state. `connection.md`'s "the pump may ignore
  control commands" is contradicted by the START/STOP writes above. Both were
  hedged in the original wording and neither is sourced to a capture.

  If a pump variant ever does need them, the failure is loud — it never reaches
  Pump Ready — and this is one revert.

- **A pump whose clock is never synced now says so, instead of silently letting
  schedule windows drift.** The pump keeps its own RTC and runs schedule windows
  off it; nothing else corrects that clock. When a sync cannot happen, the
  symptom is a schedule firing at the wrong hour days later, with every sensor
  still reporting healthily.

  Three lookalike states were collapsed into one silent retry. `time_id` not
  configured is permanent — the option is optional in the schema and was absent
  from every document in the repo. A configured `time_id` whose source never
  answers is the *likelier* failure, precisely because both entry packages set
  the option: a `homeassistant` time platform on a node that cannot reach Home
  Assistant looks configured and never produces a clock. And a source that
  simply has not answered *yet* is normal at boot and must stay quiet. Only
  elapsed time separates the last two, so a silent source is reported once it has
  stayed silent for 15 minutes while the transient case says nothing. A missing
  `time_id` needs no window — no amount of waiting can change that answer — and
  is reported from the first check. Both repeat at most hourly.

  Neither permanent state drives the retry loop any more. That loop deliberately
  does not stamp an attempt when nothing was written, so a sync blocked by a pump
  that is not yet synchronized is retried on the next poll rather than backed off
  fifteen minutes — correct for a condition that resolves itself, a spin when
  applied to one that cannot. Every 10-second poll walked the full path to fail
  at the same place, forever.

  On the cost of that spin: at the INFO level this component ships at, ESPHome
  compiles `ESP_LOGD` out entirely, so it was a few compares per poll and no log
  output at all — which is also why the old reports were invisible rather than
  merely quiet. On a node built at `logger: level: DEBUG` it was four lines per
  poll across 8,640 polls a day, each one an API frame fanned out to every
  subscriber. An earlier draft of this entry claimed the API-frame cost applied
  at INFO; it cannot, and the two halves of that argument were mutually
  exclusive.

  The startup warning reaches the serial console only — component setup runs at
  `setup_priority::DATA`, long before the API server accepts a log client, and
  ESPHome keeps no backlog for late subscribers — so the hourly repeat is what an
  `esphome logs` session actually sees. An earlier draft warned only at startup
  and would have been invisible to most users, which is the same defect it set
  out to fix. The repeat runs from the poll, so it needs the pump link to be up;
  a node that has never connected reports on serial alone.

  Two limits worth stating rather than implying. `settimeofday()` is one way, so
  a source that answers once and then dies leaves the system clock free-running
  and valid — the pump keeps being written from a drifting ESP RTC and this
  warning cannot see it; what is detected is a clock that was never set, not one
  that stopped being corrected. And the grace window is measured from boot, so a
  node whose BLE link takes longer than 15 minutes to come up has no window left
  on its first check and may emit one pair of lines before settling.

  `time_id` is also now in the `alpha_hwr` options table in
  `docs/configuration.md`, with a section on what breaks without it. The packages
  set it, so only configs writing an `alpha_hwr:` block by hand were exposed to
  the missing-option case — the silent-source case reaches everyone.

- **No example configuration carries a credential that would work if flashed.**
  `hwr-pump-example.yaml`, `hwr-pairing-example.yaml` and
  `hwr-pump-schedule-example.yaml` each inlined a real API encryption key and a
  real OTA password. Both were flagged in comments, and both were live in
  anything flashed unchanged — an encryption key published in a public
  repository is not encryption, and a published OTA password lets anything on
  the LAN flash the node. All three now read WiFi, the API key and the OTA
  password from `secrets.yaml`.

  **Moving them into the template was not, by itself, a fix**, and the first
  draft of this change claimed otherwise. The documented path is `cp
  secrets-example.yaml secrets.yaml`, and that template shipped the same
  published key — so the recommended sequence still produced a node secured with
  a key anybody can read, now with the warning one file further away. So the
  template's three security-critical values are deliberately unusable: `api_key`
  is not valid base64 of the right length, `ap_password` is shorter than the 8
  characters WPA requires, and `ota_password` is commented out. Copying and
  building now fails with an error naming whichever you have not set. CI no
  longer copies that template; it writes its own throwaway secrets with a
  freshly generated key, so the tree contains no working key at all.

  `ap_password` is on that list because `hwr-pump-example.yaml` pairs the
  fallback hotspot with `captive_portal:`. A password published here would let
  anyone in range join that recovery network and reach the portal — an opening
  rather than a formality, and one an earlier draft of this entry left standing
  while claiming the examples were covered.

  Two of the guarantees needed narrowing rather than strengthening. Deleting
  `ota_password` from the secrets file does *not* produce unauthenticated OTA,
  as an earlier draft of the template said: every example references `!secret
  ota_password`, so the build fails instead. Genuinely running without OTA
  authentication means removing the `password:` line from the example's `ota:`
  block. And `components/alpha_hwr/discovery_example.yaml` is nested, so ESPHome
  resolves `!secret` beside *it* rather than at the repository root — giving it
  an encryption key created a config the documented single `cp` could not build
  until the second copy was written down.

  **Two examples were worse off than the three being fixed**, which the first
  draft also missed while asserting in the README that every example was
  covered. `dhw-demand-example.yaml` and `components/alpha_hwr/discovery_example.yaml`
  declared a bare `api:` with no encryption whatsoever and, in the first case, an
  `ota:` with no password — an unauthenticated API and unauthenticated OTA,
  rather than merely a published one. Both now take the same secrets.

  One behaviour worth stating because ESPHome does not warn about it: an *empty*
  `ota_password` is accepted and silently disables OTA authentication entirely.
  The template says so at the point of use, and the README repeats it.

  `hwr-pairing-example.yaml` and `hwr-pump-schedule-example.yaml` also shipped
  `logger: level: DEBUG`, against advice a sibling example already carried:
  every log line is an API frame fanned out to every subscriber, which is the
  heap-exhaustion path in issue \#127. Both are now commented out as an opt-in.
  `discovery_example.yaml` keeps DEBUG, because printing scan results is the
  entire purpose of a config you flash once to learn a MAC address — now with a
  note saying not to carry that level into anything left running.

  The `tests/ci-compile*.yaml` harnesses keep a placeholder key deliberately:
  they run before CI seeds any secrets, they are not a recipe anyone is told to
  flash, and giving them `!secret` would only reorder the workflow to no benefit.

  `secrets-example.yaml` gains `ap_password`, which the fallback hotspot in
  `hwr-pump-example.yaml` needs and the template never defined, and the README's
  examples section leads with the `cp` step — which no document mentioned at all
  while the examples were merely insecure rather than incomplete.

- **The component no longer answers other devices' pairing requests, or acts on
  their pairing results.** BLE GAP events are broadcast rather than routed:
  ESPHome hands every GAP event to every BLE client and then to every node, so
  the five security branches here saw the pairing traffic of every other peer
  sharing the node — a Bluetooth proxy connection, a second `ble_client`,
  anything — and all five acted on it. ESPHome's own client checks the address
  before acting on the same events; this component did not, and so undid that
  filtering one stack frame later.

  Two branches replied "yes, let's pair" on a stranger's behalf. Neither
  consulted `enable_pairing`, which defaults to false and means passive
  telemetry only — the security setup honoured it, the reply paths did not.

  The third is not hardening. The auth-complete handler read the address only to
  print it, so a stranger's result was taken as the pump's: on failure it
  latched that stranger's reason at the rank that outranks every other fault
  reason — masking the real cause — and, with encryption in flight, disconnected
  the pump. A stranger's *success* is as wrong: it publishes pairing-OK for the
  pump, clears a genuine pairing fault, and can release a held-back notification
  subscription onto a link that never authenticated.

  One asymmetry is documented rather than papered over. At a security request
  ESPHome's own client has already consented for its configured peer,
  unconditionally, so declining here would send a contradicting answer into an
  exchange already underway; `enable_pairing` governs what this component does,
  not whether `ble_client` consents. At a numeric-comparison request nothing
  else responds by default, so the refusal is real — and it is a behaviour
  change worth naming: on a node with `enable_pairing: false`, a numeric
  comparison request from the pump is now refused where it was previously
  accepted. Reaching it takes a deliberate `io_capability` change, because
  ESPHome's BLE stack sets "no input, no output" globally, which forces Just
  Works and suppresses the request entirely.

- **A BLE write that fails part-way through a packet no longer feeds the next
  command into the wreckage.** The failure path was identical whether zero or
  twenty bytes had reached the radio, and the 50 ms send pacing is measured from
  the last *successful* chunk — so it was already satisfied at the moment of
  failure and the next command went out on the following tick, ~16 ms later.

  The peer is holding the head of a frame whose length byte promises more. A
  receiver built like this component's ignores a frame start while reassembling
  and only abandons a partial after a second, so it appends whatever arrives and
  keeps appending until the declared length is reached. For commands that wait
  on a response the cost is the next one; for the fire-and-forget reads that
  queue back to back (telemetry, the auth handshake) a probe against this
  component's own reassembler swallowed four.

  A hold is now armed only when bytes actually reached the wire, and blocks the
  next send until the peer's partial must have gone stale. Its length is derived
  from this component's own reassembly timeout, which is an estimate of the pump
  rather than a measurement — the pump's reassembler cannot be observed from
  here. The stall is paid only on a fault.

- **The pump clock sync reported success without checking anything, and the
  "Last Clock Sync" sensor recorded it.** `TimeService::set_clock_async()`
  formatted its frame, handed it to the transport as fire-and-forget, and then
  called back `true` unconditionally — under a comment promising a verification
  read that was never written. That bool is what stamps the sensor and what
  arms the 24-hour suppression timer, so a sync that never left the node showed
  a fresh timestamp and then declined to try again for a day, with the pump
  running its weekly schedule off whatever clock it happened to hold.

  The write is now a `SET_CLOCK` operation in the write layer, which is where
  AGENTS §6 has said every pump write belongs since issue #92 — it was the last
  standalone write path left after remote mode moved. It is confirmed by
  reading the pump's own clock back (Object 94 Sub 101) and comparing it with
  the node's clock at that same moment. `accepted` means the pump holds the
  right time, which is the question worth answering: a pump that was already
  correct settles accepted. A clock that still disagrees settles `rejected`
  with the offset in `detail`; a readback that will not decode settles
  `timeout`, because a clock we cannot read says nothing about whether the pump
  is wrong — including a pump whose RTC a power cut has reset to the year 2000,
  which decodes perfectly and is exactly what a sync exists to repair.

  The accept window is asymmetric, which took a bench capture and three
  adversarial passes to get right. The time inside the frame is built when the
  operation runs, and the transport sends one command at a time, so a frame
  queued behind other traffic arrives late and leaves the pump genuinely a few
  seconds behind — the node's own latency, reported as the pump's drift. What
  bounds it is the operation's own age, so a pump reading behind by no more than
  that is accepted (with the real offset still reported) while a pump reading
  ahead is held to the flat 5 s tolerance. Both sides of the comparison are
  resolved from local fields by the same function, which matters for one hour a
  year: inside the DST fall-back fold an exact epoch and a re-resolved one sit
  3600 s apart, and the difference settled a correct sync `rejected` every 15
  minutes until 02:00.

  It emits a `set_clock` settle event (`origin: internal`, `clock_offset_s`);
  there is no service, since nobody calls this — the periodic check does. A sync
  that does not confirm now retries in 15 minutes rather than a day. Stamping
  the retry timer at submission rather than on success is what keeps that
  honest: `update()` runs every 10 s, so throttling on success alone would have
  turned an unconfirmable clock into a write every 10 seconds.

  Two behaviour changes worth stating. Going through the write layer means the
  sync now waits for the same readiness gate as every other write, so it happens
  on the first poll after the link is fully synchronized rather than 2 s into
  the boot read chain — that leg only ever measured drift anyway, and its
  attempt to write was refused 100% of the time. And a node with no `time_id`
  no longer logs about it at all above DEBUG.

  The APDU is byte-for-byte what it was. Its six leading bytes had been carried
  as an opaque "Type 322 header" with the object and sub-id labelled backwards.
  They are named now — object 94, sub-id 100, type 321 version 2, an 11-byte
  size — from the identical layout of the capture-verified Object 91 Sub 421
  write, cross-checked against a measured identifier table which pins the
  *read* at type 322 version 1. Config and actual are two objects; the old label
  took the read's type and pasted it on the write.

  Both frames are bench-captured rather than inferred, and the fixtures are
  transcribed from the capture — including five trailing bytes in the reply that
  the parser ignores, because a fixture carrying only what the parser reads
  proves the parser can read its own fixture. The reply's first three bytes were
  documented as `[Status(2)][Length(1)]` and are an ordinary size header.

  Thirteen host tests drive the shipped operation against the pump simulator,
  which learned Object 94 and a clock that runs with mock time; the suite builds
  with `-DUSE_TIME` so `TimeService::current_time()` is the real function rather
  than its stub. None of this had any host test before — `time_service.cpp` had
  never been compiled by the suite at all, which is why the mock ESPHome SDK
  gains an `ESPTime` with its fidelity boundary written down. `mutation_check.sh`
  gains eight entries, six of them for code that three earlier passes had left
  unguarded.

- **`link_max_gap` no longer censors its own sample at the threshold it exists
  to validate** (issue #176, review of PR #189). The statistic only counted
  intervals that ended in a notification, and an interval that ends in a
  watchdog recycle never does: the watchdog re-arms the window and disconnects,
  so nothing closes it. Every excursion past `data_timeout` was therefore
  discarded and the running maximum asymptoted to just under the budget whatever
  the pump did — "never above 12 s in a month" reading as "60 s was comfortable"
  when what it meant was "no quiet period between 12 s and 60 s ended on its
  own". Measured on the bench by flashing the old build and the new one against
  the same pump with `data_timeout` forced to `5s`: five recycles, and the old
  build's maximum read **2.6 s** — apparent double headroom over a budget it had
  breached five times. The new one reads **6.0 s**.

  An interval ended by a plain drop — supervision timeout, pump power loss, the
  encryption-failure teardown — was discarded the same way, censoring the sample
  at a threshold nobody configured. Same A/B, with polling suspended for 45 s on
  a live link and the link then dropped: the old build published nothing and sat
  at its steady-state 9.5 s, the new one recorded **53.0 s**.

  Both kinds are recorded now, at the recycle and in the disconnection callback.
  A disconnect with no open before it samples nothing, so a failed connection
  attempt cannot record the downtime since the last session as if the link had
  been up and silent for it.

  The open-to-first-notification interval is now sampled too, rather than
  excluded as "the handshake, not the pump's cadence". It is inside the window
  the watchdog acts on, and per the sizing note it is the *binding* case — 17.2 s
  worst case against a 60 s budget, where steady state is bounded by our own 10 s
  poll — so excluding it left the statistic unable to report the case the default
  is tightest against. Nothing visible changes on a healthy link: the maximum
  reaches the 10 s poll interval in the first poll cycle regardless — 4.9 s then
  9.5 s on the bench, where the first figure is the newly-sampled handshake
  interval and the second is the first poll cycle overtaking it.

  All of these biased the number downward, toward "the budget was never close":
  the direction that argues for keeping a default nobody has validated. What a
  reading means is now stated where it can be read — it is a lower bound on the
  quiet, and because the backoff widens the window in force and the reconnect
  does not reset it, a value above the configured budget does *not* by itself
  mean a recycle happened. `link_recycles` is what distinguishes the two.

  The sampler moved into `link_watchdog.h` as `LinkGapSampler` so host tests can
  pin it, and `tests/test_link_watchdog.cpp`'s simulator now models the backoff
  it had been ignoring — which corrected a neighbouring test that claimed the
  watchdog re-fires once per tick during the async disconnect. At the configured
  budget the backoff covers that on its own; the re-arm is load-bearing at the
  one-hour cap, where there is nothing left to double, and that is what the test
  asserts now.

- **A single-event slot past what the pump actually has now settles REJECTED
  immediately instead of TIMEOUT fifteen seconds later.** Single events live at
  Object 84 SubIDs 900–999, so any slot 0–99 is a legal thing to *ask* for — but
  how many exist is a property of the device, reported in its ClockProgramOverview.
  Ask a five-slot pump to clear slot 10 and SubID 910 went out to nobody; the
  operation then sat on the watchdog and blamed the link for what was really a bad
  argument.

  The bound reads the pump's own count rather than a constant, and it sits after
  the overview precondition so that count is known. On the bench pump — five
  slots — slot 10 and slot 5 are refused in about 0.3 s with `single event slot 5
  out of range (pump has 5)`, and slot 4 still writes normally.

  A slot of 100 or more is refused earlier still, and separately, because it is
  wrong on *every* pump rather than merely on this one: SubID 900 + 100 is 1000,
  which is where the weekly schedule's layer records live. That check runs ahead
  of the overview read, so a broken link no longer turns a bad argument into
  `"overview not readable"` — the same ordering `set_schedule_entry` already used
  for its layer and day. A slot the protocol allows but this pump lacks still
  reports the link failure when the link is down, deliberately: the count comes
  from the pump, so with the pump unreachable there is no count to range-check
  against.

- **Single-event readback resolved its timezone offset from the wrong clock,
  so writes near a DST transition settled REJECTED.** The pump stores
  single-event timestamps as local Unix time, so both directions shift by the
  local UTC offset — but the helper that resolves that offset takes a *UTC*
  instant, and the read path was handing it the local value straight off the
  wire. That evaluates the offset a whole offset away from the true instant
  (seven hours early in PDT), so any event within that window resolved to the
  wrong side of the transition and came back an hour out. The write itself was
  fine; the readback disagreed with it, and the confirm comparator reported
  REJECTED while the pump held exactly the right value.

  The read path now resolves the offset from an approximate UTC and refines it.
  Measured at 15-minute steps across a 24 h window at both 2026 US Pacific
  transitions: spring-forward goes from 28 of 97 samples wrong to 0, fall-back
  from 32 to 4.

  The remaining 4 are not a shortfall. Fall-back *repeats* an hour of local
  time, so two distinct UTC instants encode to the same wire value and no decode
  can recover which was meant — a property of the pump storing local time, which
  its own clock program shares. The residual is exactly
  `[transition, transition+1h)` once a year, against seven-to-eight hours at
  *both* transitions before. An event inside that hour still settles REJECTED,
  now correctly reporting a value that genuinely cannot be confirmed.

- **`data_timeout` recycles now back off instead of repeating forever.** A link
  that stayed deaf was recycled every ~66 s indefinitely — roughly 1,300 passes
  a day, for as long as the condition lasted. No single recycle is wrong; the
  unbounded repetition is, because each one re-enters the encryption-on-open
  path on a bonded pump and so takes one more run at the window where an
  encryption request can fail and erase the bond.

  A recycle that produces no data now doubles the window for the next one, up to
  a ceiling of one hour, and a notification received once the session is ready
  resets it to the configured value — ready-gated because a deaf pump still
  answers the handshake, so resetting on those frames would clear the window
  once per session and the backoff would never engage. A widened window is not
  reset by the reconnect either, so it also governs the next connection's
  handshake. A link that can recover still does on the first or second
  try; a permanently deaf one drops to about 28 recycles a day. A `data_timeout`
  of `0s` stays disabled, and a budget configured larger than the ceiling is
  returned unchanged rather than clamped down (issue #176).

- **A failed notification subscribe now names itself instead of being
  rediscovered as a timeout.** `subscribe_to_notifications()` has six terminal
  paths and discarded the answer on five. Four returned early after logging —
  no client, no service, no characteristic, `esp_ble_gattc_register_for_notify`
  failed — and since `subscribed_callback_()` is the only thing that advances
  the session out of `SUBSCRIBING`, those four parked it there. The fifth, a
  CCCD write that failed synchronously, fell through to the *same* callback as
  success, so a failed subscribe was indistinguishable from a good one.

  All five were caught by the 60 s data watchdog, and that design is unchanged:
  one timer covers every cause, including a link that goes deaf later, which no
  return code can see. What it could not do is say *which* thing failed, or say
  anything at all for a minute. Each failure now latches its own reason on the
  Pump Link Fault surface at the moment it happens, so a missing characteristic
  reads as one instead of as "No data from pump (60s)".

  The reporting is wired at the sites where those causes actually arise — the
  service-discovery and characteristic-lookup paths — not only inside the
  subscribe call, which those paths pre-empt with the same lookups.

  It does **not** force an early disconnect. The subscribe decision point is
  ~2–3 s after connection-open and a bonded reconnect re-arms in ~2 s, so
  recycling there would run a ~6–8 s cycle against the watchdog's ~66 s — around
  nine times more passes through the encryption-on-open window where a failure
  can erase the bond. Naming the cause is the safe half; choosing the recycle
  cadence belongs to the watchdog, which now backs off.

  A failed CCCD write is recorded but does not *hold* its reason over the
  disconnect that may follow: one of its documented synchronous causes is the
  link already being gone, and holding would relabel a link loss as a subscribe
  fault for the whole reconnect.

  The decision lives in `subscribe_outcome.h` with host tests, following
  `link_watchdog.h`, because `ble_connection_manager.cpp` is compiled by no host
  test and anything expressed there is unverifiable (issue #175).

- **A pairing failure is no longer overwritten by the watchdog 60 s later.**
  The hold that keeps a named cause on the Pump Link Fault surface was written
  as a list of exceptions — the watchdog wrote its generic "No data from pump"
  unless a *subscribe* hold was in place. An auth-failure hold is neither that
  nor no hold at all, so it was overwritten: the same overwrite the subscribe
  hold exists to prevent, on the one hold that records a bond-erasing failure.

  Worse than losing the string. The overwrite also relabelled the origin as the
  watchdog's, and that origin is released by inbound data — which an unbonded
  pump keeps sending after a failed SMP, since passive telemetry needs no bond.
  The pairing diagnostic would then be erased by the very next notification,
  which is precisely the case the origin was introduced to protect.

  The rule is now a rank (`failure_hold.h`, host-tested): `NONE < DATA <
  SUBSCRIBE < AUTH`, and all four write sites ask the same question, so a hold
  added later needs no new exception anywhere. Equal ranks still overwrite, so
  a repeated watchdog fire keeps refreshing its own text and the backoff
  escalation ("(60s)", then "(120s)") stays visible. The release rules are
  deliberately *not* unified: inbound data releases the data and subscribe
  holds and must not release an auth hold, which a successful `AUTH_CMPL`
  clears. Each is a switch, so a new hold cannot be added without deciding what
  releases it.

  **An auth hold is now also released when the session reaches READY**, which
  is what stops the rank from making it permanent. Nothing outranks it, and a
  pump that never pairs successfully never produces the `AUTH_CMPL` that clears
  it — so on the old rule it would have masked every later fault for the rest
  of the boot, while no longer being able to report the pairing failure at all:
  the fault string is displayed only while the session is *not* ready. Past
  READY the hold could only put a stale pairing reason in front of the next
  outage's real cause. Pairing state itself is unaffected — it has its own
  sensor. READY is a deliberately weak signal (a deaf link reaches it too), so
  it releases *only* the auth hold: releasing the watchdog's there would clear
  the deaf-link reason on exactly the links it describes.

  One reporting change worth knowing: a failed CCCD write is recorded at the
  lowest rank, so while another fault is held its string no longer replaces
  what is shown. It is still logged, and it never held its reason anyway.

  Follow-up to the hold introduced by PR #188 (issue #175).

  No behaviour change on a healthy link, and no released firmware is known to
  have hit the overwrite: it needs an auth hold and then 60 s of silence, and
  the bonded encryption-failure path disconnects without going through the
  watchdog's writer.

- **`dhw_demand`: the `dhw_in_use` tier no longer overrules a measured
  no-draw.** The heater's in-use flag fired whenever the tiers above it
  declined — including when the subtraction had declined *because it measured
  no household draw*. A tier returning without setting demand is not a veto,
  but the effect on what gets published is identical to one, so a flag was
  outranking a measurement.

  Replaying 30 days of stored sensor data through the companion detector: of
  1007 cells with the flag sustained, 937 had the subtraction available and
  **all 937 measured no draw** (median −0.021 GPM, max +0.134 against a 0.30
  cut). Not one *measured* draw reached this tier, because the subtraction
  already answers those — so what the tier was adding there was recirculation.

  Stated as "measured" deliberately: the only instrument that could confirm a
  real one is the subtraction being questioned. What the gate gives up is a true
  draw whose measured value lands at or under the cut — with the subtraction's
  −0.10 ± 0.06 GPM offset, a real draw up to roughly 0.4 GPM — ruled out by one
  house over one month. The trade is the right one, but it is a trade, not a
  free correction. Downstream that removed 176 minutes of published demand and 61
  sessions, and un-backdated 13 more that phantom pump-on demand had bridged
  into. Every corpus-confirmed true positive survived; they all live in the
  no-measurement cells the tier still fires on, which is what it exists for.

  Past the gate the subtraction is by definition unavailable, so the
  measurement-derived intensity arm goes with the cells it applied to. It could
  only ever run on a value at or under the threshold, publishing levels as low
  as 0.08 — an order of magnitude *below* the no-claim constant, which asserts
  near-zero draw rather than declining to assert
  ([issue #173](https://github.com/eman/esphome-alpha-hwr/issues/173)).

- **Every device-info string was missing its first character.** The Class 7
  ReadString response has a six-byte header — `[STX][LEN][DST][SRC][0x07]
  [Count]`, with the string starting at byte 6 — and the parser assumed seven,
  reading from byte 7 and sizing the string at `len - 9`. The two version
  strings, which had no compensating patch, therefore reached Home Assistant
  short: `2601618V04.02.01.02539` and `2811431V06.00.01.00001` against the
  `92601618V04.02.01.02539` and `92811431V06.00.01.00001` the Grundfos GO app
  displays for the same pump. Both now match, confirmed on hardware.

  The two long-standing string repairs turn out to have been this bug's own
  footprints, not the pump's. Prepending `A` to `LPHA HWR` restored the `A` at
  byte 6. Prepending `1` to a serial starting `0` was never a fix at all: it is
  correct for a serial beginning `10` and would corrupt one beginning `20`.
  Both are removed. Published serials are unchanged — the old path reached the
  right answer for this serial by coincidence, and now reaches it by parsing.

  The host test could not have caught this, because its fixture builder was
  written from the same seven-byte assumption the parser made, so the two
  agreed and the pump took the blame. Fixtures are transcribed from a capture
  now, and each is checked against the bytes the dump shows before it is used
  (issue #179).

- **The schedule card treated every write as successful.** `_saveChanges`
  discarded the user's pending edits at call time, then re-read the device on a
  3 s timer. A `set_schedule_entry` carries a 20 s watchdog and writes are
  queued, so the read returned pre-write state and overwrote the edit with the
  value it was meant to replace — and a write the device *rejected* looked
  exactly like one it accepted, because the edit was gone either way. The card
  already generated a unique `op_id` per call and the device already answers
  every write with exactly one `esphome.alpha_hwr_write_settled` carrying it;
  the card simply never subscribed. It does now: an edit stays on the grid
  until its own write settles, failures are named on the card with the device's
  own detail string, and the refresh fires when the batch drains rather than on
  a timer. An edit re-made while its write is in flight survives the older
  write's confirmation.

  A settle event can legitimately never arrive — a Home Assistant restart, a
  websocket reconnect, a node reboot — so a backstop releases the wait after
  the firmware's own watchdog budget has had time to fire, and says that is
  what happened rather than reporting success. It errs long on purpose. The
  per-command budgets bound an operation's time *at the head* of the device
  queue — `arm_watchdog_` runs from `start_front_`, so a queued operation
  carries no timer until it gets there — and the queue is shared with every
  other write source, including the card's own untracked refreshes. A backstop
  that fires early is the worse failure: it reports "no confirmation" for a
  write still in progress, then discards the real settle when it arrives. The
  wait also re-arms as writes confirm, so it tracks what is still outstanding
  rather than what the batch started as.

  The fixed slack is not a bound on the queue, and cannot be: the queue has no
  depth limit and a queued operation carries no watchdog until it reaches the
  head, so enough foreign writes in front would outlast any constant. What
  closes that is a signal rather than a bigger number — every operation that
  completes on the node fires a settle event, so the deadline re-arms on
  observed queue progress and the slack only has to cover a single long
  operation that has not finished yet.

  A card that is re-attached mid-write — which Lovelace does whenever a masonry
  view re-columns — restores both its backstop, on the original deadline, and
  its settle subscription. Without the first, teardown cancelled the only timer
  that could ever release the write and the card rendered "saving…"
  permanently with Save and Discard both disabled. Without the second, a write
  settling before Home Assistant's next state push would land on a card that
  had stopped listening, and be reported as a failure.

  The single-event paths had the same defect against a 60 s watchdog, plus an
  optimistic local delete that made a failed clear look like a success for the
  better part of a minute. Both are fixed the same way.

  Not changed, deliberately: the card still writes one entry per changed cell
  rather than batching through `upload_schedule`. `build_layer_image` clears
  every cell an upload does not list, and the card silently skips a layer
  sensor that is malformed or not yet cached — so batching would turn a stale
  read-back into data loss. Ordinary edits are one to three writes, fewer than
  an upload costs.

- **A DHW draw that stopped mid-recirculation went on being reported until the
  pump stopped.** The pump-on continuation tier presumes a draw established
  just before the pump started is still running, and its only test for "still
  drawing?" was household flow above 0.3 GPM. While the pump runs that meter is
  reading the recirculation loop, and every pump-on value recorded here clears
  0.3 by more than 2x -- 0.71 GPM at the pump's 1650 RPM clamp floor, a 1.31
  no-draw median, 2.22 at the no-draw p90 -- so the exit was unreachable. A
  1.80 GPM draw stopping five minutes into a thirty-minute run held
  `dhw_demand` true at 0.85 confidence for the remaining twenty-five, with
  `session_duration` reporting 1870 s against a 360 s draw, while the
  subtraction sat there reading 0.00 GPM.

  This was not a regression and not a rule violation -- the code implemented
  the spec as written, and `AGENTS.md` §11.4 documented the raw-flow term
  verbatim, complete with its own exemption from the "no pump-on rule may key
  off raw meter flow" prohibition. The hole was in the spec, and the evidence
  for the prohibition is the same evidence that condemns the exit: if no
  threshold on raw meter flow can separate a draw from recirculation, none can
  detect one *ending* either.

  The tier now releases when the subtraction -- the only pump-on measurement of
  household draw there is, under its own existing guards -- reads at or below
  `pump_on_demand_flow_threshold`. That release also retires the stored
  evidence, so a later loss of the subtraction cannot resurrect a claim that
  has already been disproved. Because the subtraction goes silent below
  `pump_on_demand_min_speed_rpm`, and a pump clamped under that floor never
  produces one at all, an expiry bounds the case where nothing can contradict
  the tier: new `pump_on_continuation_max_seconds`, default 300, `0` to disable
  the tier outright. A draw that is real *and* measurable is picked straight
  back up by the subtraction, so the expiry costs recall only where nothing
  could see the draw anyway.

  Both retirements log at `INFO`. They are once-per-continuation by
  construction, and the default level is `INFO` (issue #127 keeps `DEBUG` off) --
  which is exactly the configuration in which a field report of "demand stayed
  on" would otherwise be undiagnosable.

  Found by the 2026-08-12 audit (finding 10). The test that claimed to cover
  this exit passed only because it used a 0.1 GPM meter reading -- below every
  value the repo has ever recorded with the pump running.

  Verified on hardware. A full draw armed the tier and closing the tap released
  it, the log naming the subtraction as what ended it, with the pump still
  turning -- so the meter was still reading loop flow above threshold, the
  condition under which the tier previously held for the rest of the run.

  The small-draw case was then measured directly, which is the run that
  matters. A 0.60 GPM draw armed the tier; reduced mid-run without being
  closed, the subtraction settled at **+0.226 GPM for 26 s** -- inside the 0 to
  0.3 band where the first version of this fix retired the capture -- and the
  tier held throughout, publishing no method change for 3 m 48 s until the tap
  was actually closed, at which point the subtraction went to -1.09 GPM and it
  released. That run also put the steady residual at -0.08 GPM (tap 0.602,
  difference 0.52), inside the documented -0.10 +/- 0.06, confirming the -0.47
  seen earlier was a deceleration transient rather than bias -- the case the
  tick count covers.

  One finding from that bench is recorded rather than fixed here. A steady
  household draw metering 0.11-0.16 GPM produced `deterministic_idle`
  throughout: it never cleared `flow_threshold` (0.3 GPM), so nothing detected
  it at all. Across 14 h of one installation, 24 sub-threshold samples fell in
  `0 < flow < 0.30` **with the pump confirmed off** -- readings where the meter
  sees only genuine demand. The meter reports a hard `0.0` at rest rather than
  dithering, so this is real signal below the floor rather than noise, and the
  obstacle to lowering it is recirculation decay passing through any threshold
  after a pump-off edge rather than sensor resolution. That needs the same kind
  of edge-proximity measurement that placed
  `latch_pump_off_suppression_seconds`, so the floor is unchanged and the
  limitation is now documented in `AGENTS.md` §11.4 and
  `docs/configuration.md`. Issue #180.

- **Device information and the operating statistics could silently never be
  read.** The one-time chain that fetches them is latched by a flag that only a
  BLE disconnect clears. It normally runs after authentication, but it also
  runs when the BLE link survives an ESP32 restart and no re-authentication
  happens — and on that path it can fire before the pump is answering. When it
  does, those reads miss, the flag stays set, and nothing runs them again for
  as long as the link holds.

  Nothing surfaces it. Telemetry polling is a separate path, so the sensors
  keep updating at their usual cadence, and the two caches that gate Pump Ready
  — the control cache and the schedule overview — are refreshed by their own
  polls rather than by this chain, so they recover on their own and Pump Ready
  comes on as usual. The node therefore looks entirely healthy while the
  product name, serial, firmware versions, start count and operating hours are
  simply absent.

  The chain is now re-armed when what it alone produces has not arrived, rather
  than when the self-healing caches are still invalid — the distinction matters,
  because those caches typically recover well inside the timeout and would
  otherwise call the attempt a success. Pending timers from the failed attempt
  are retired first so the retry cannot double up with them, the interval
  doubles from 60 s to a 10 minute ceiling so a pump that never returns these
  values is not re-read forever, and the pump-clock write in the chain runs on
  the first attempt only, leaving that write throttled to once a day as before.
  The retry itself is deliberately unbounded — a limit would restore the same
  permanent stall it exists to prevent.

- **A pump that stopped answering could report itself connected forever.**
  Nothing in the connection sequence checked that data was coming back:
  authentication is a chain of timers that sends its packets and declares
  success 1.2 s later without inspecting a single reply, and of the six paths
  through notification subscription, four give up silently while the other two
  — CCCD write failed and CCCD write succeeded — both continue identically, so
  a failed subscription is indistinguishable from a good one. A link whose GATT
  writes succeed but whose notifications never arrive therefore reached the
  ready state and stayed there.

  The link-status ladder could not catch it either — its first rung is "session
  is ready", which refreshes the very timestamp the "Unreachable" rung below it
  measures, so a deaf link kept itself healthy-looking indefinitely. What the
  user saw was Connected and Pairing on, Pump Ready off, every sensor frozen at
  its last value, and a control-cache retry every 5 s that never completed.
  Nothing recovered from it, because recovery is driven by BLE disconnection
  and the BLE connection was fine.

  A new `data_timeout` option (default `60s`, `0s` disables) drops the link
  when nothing has been received for that long, letting the normal reconnect
  run. It is a liveness check rather than a gate on becoming ready: this pump
  does answer during authentication, but that is one specimen, and a variant
  that stayed quiet until first polled would otherwise never become ready at
  all. One timer covers every cause — both CCCD failure paths, the four
  subscription paths that give up silently, and a session that goes deaf
  mid-run. A ready link is polled every 10 s, so in steady state the default
  tolerates five missed cycles and acts on the sixth; the tightest case is the
  handshake, which reaches ready in about 6 s and sees its first data within
  ~17 s worst case.

  What this fixes is a link that *can* recover. It does not make a permanently
  deaf pump legible: because becoming ready still does not require data, such a
  pump cycles between ~60 s of looking connected and ~6 s of reconnecting, so
  the status mostly still reads Connected and both link sensors flap once per
  cycle. The **Pump Link Fault** sensor reads `No data from pump (60s)` during
  the reconnect — held there rather than overwritten by the local disconnect
  that caused it — and returns to `None` on recovery.

- **The pump's control-mode read was matching its response through a fallback,
  not on its own terms.** The Object 86 Sub 7 read -- which drives control mode,
  run state, remote/local control source and the per-mode setpoint cache --
  passed its two response-matching arguments in the wrong order, so its primary
  comparison never succeeded. It matched only via a "backup" branch in the
  transport that existed to absorb exactly that mistake. The call site is fixed,
  the fallback is gone, and the ordering is pinned in two places: at the
  transport, and at the call site itself, where a swap now names its own cause
  instead of surfacing as 36 unrelated-looking write failures.

  The underlying confusion is that a GENI response carries **no Object ID and no
  Sub-ID**: bytes 6-9 are `[00][TypeH][TypeL][Version]`, the object type and
  version. Byte 6 is `0x00` in 100% of the 21,236 captured Class 10 responses
  long enough to carry a type header, and bytes 7-8 always equal the type the
  request named. The fields are renamed
  accordingly and the wire layout is documented where it is parsed, because the
  old names had already caused one incorrect analysis.

  Also removes an `opspec == 0x02` branch: byte 5 is the APDU body length in the
  response direction, not an operation code, so that test selected a 10-byte
  frame rather than a packet format -- and no such frame can reach it, since the
  `len < 11` guard above returns first. Zero of 21,720 captured Class 10
  responses had it.

- **Command responses are now CRC-checked.** Only telemetry validated the frame
  CRC; the command-response path did not, so every control, schedule,
  single-event, event-log and device-info payload was parsed from bytes nothing
  had verified -- including the readbacks that decide write verdicts. A runt
  produced by a mid-frame fragment, or any radio corruption, could satisfy the
  class/object match and be taken for the answer to a queued command. The check
  sits at the one point where a frame is complete, so it covers the dispatch
  path and the general packet callback together, and the frame is trimmed to
  its declared length first because trailing bytes are outside what the CRC
  covers -- which also fixes a real misdispatch, since two frames arriving in
  one notification were previously handed to the packet callback fused into a
  single oversized packet. Verified two ways: replaying 36,394 captured BLE
  notifications through the transport completes 17,624 frames and drops 3
  (0.017%, all genuine corruption -- no alternate CRC window matches them);
  and on the pump, zero drops across four minutes of live traffic with
  telemetry, both multi-frame read chains and a write all confirming
  normally.

- **Remote mode goes through the write-operation layer, and is confirmed by a
  readback instead of by the command ACK.** It was the last write in the
  component that reached the transport on its own: two standalone
  `enable_remote_mode()`/`disable_remote_mode()` entry points with no settle
  event, no watchdog, no supersede handling, and a verdict taken from the Class
  3 ACK byte. That verdict was wrong in both directions — a pump that acks the
  command cleanly and then stays on Local/Panel was reported as remote-enabled,
  and a pump that applied the command but whose ACK window closed was reported
  as unchanged. `set_remote_mode` now settles on the pump's own `control_source`
  byte (Object 86 Sub 7), so the first case settles `rejected` and the second
  `accepted`. Toggling the Remote Mode switch emits a `set_remote_mode`
  `write_settled` event carrying `remote_enabled`; there is still no
  `pump_set_remote_mode` service. The confirm requires a recognized
  `control_source` to have been observed *since* the command, not merely at
  some point: Sub 7 reports the prioritized source after remote/local/alarm
  arbitration and the profile defines many values that are neither
  Remote(2) nor Local(1), so a reply carrying one of those leaves the cache
  untouched. Confirming against the cache alone would settle a *disable*
  `accepted` off a Local reading taken before the command, and report a write
  that did take effect as `rejected` off the same stale reading; an
  uninterpretable source now settles `timeout` in both directions.

- **The Remote Mode switch shows "unknown" until a control source has been
  read**, matching Schedule Enabled and Temperature AutoAdapt. It previously
  reported OFF on a fresh connect whether the pump was on Local/Panel or
  simply had not been read yet, so a scene reasserting "Remote Mode off"
  against a cold cache issued a write with nothing to confirm against.

- **The single-event, vacation, event-log, history and cycle-timestamp text
  sensors are change-gated.** All five republished a byte-identical string on
  every refresh service call and every reconnect, costing an API frame per
  subscriber each time for no change (issue #127). The single-event pair had two
  publish sites -- the read path and the write-settled path -- and gating only
  the first left the behaviour unchanged, which is how the bench caught it.
  Measured on hardware: five consecutive `refresh_single_events` calls produced
  five republishes before and zero after.

- **Three sequential-read chains no longer leak their whole closure graph.**
  `HistoryService::read_trends_async`, `EventLogService::read_entries_async` and
  `ScheduleService::read_single_events_async` each drove their read through a
  `shared_ptr<std::function>` that captured *itself*, so the refcount never
  reached zero and the closure, its captured result vector and the caller's
  completion callback were stranded on every invocation. The chains re-run on
  every authenticated reconnect, and the single-event one also on every
  `refresh_single_events` service call, so the loss was unbounded: roughly
  1.25–1.45 KB per reconnect cycle on the ESP32-C3, or 150–260 KB/hour under a
  flapping link, against ~150–250 KB of usable heap. Nulling the pointer in the
  terminal branch is *not* sufficient — `Transport::reset()` clears the command
  queue without invoking callbacks, so a chain abandoned by a disconnect never
  reaches that branch. The fix holds a `weak_ptr` in the outer closure and lets
  the transport command queue own the only strong reference. Bench-verified:
  40 consecutive `refresh_single_events` calls move Min Free Heap 0 bytes.

- **The Lovelace schedule card could not write to the device at all.** Every
  service registered by `api_bridge` declares an `op_id` argument, and Home
  Assistant registers ESPHome user-defined service arguments as required, so all
  seven of the card's calls were rejected before reaching the pump — silently,
  since nothing caught the rejected promise. The card also cleared its pending
  edits before any confirmation, so the grid painted an edit as applied and then
  reverted on the next sensor update. Separately, its single-event parser was
  anchored in a way that could not match the ` (run)`/` (off)` suffix the
  firmware appends, leaving the Quick Run list and today's overlay permanently
  empty — which also made single-event deletion unreachable, since that action
  is only offered on a rendered row. And a block dragged to the right edge
  serialised as hour 24, which the API rejects, so "run until midnight" could
  never be saved.

- **A timed-out `set_pump_enabled` could stop a running pump on the next
  unrelated setpoint write.** The commanded run state was cached as
  authoritative with no pending marker, and nothing rolled it back on timeout,
  so `with_resolved_enabled_state()` short-circuited on an unverified value and
  the next setpoint write folded it into the fused control frame. Only one byte
  of that frame differs, and no extra frame is sent, so nothing downstream
  noticed. The cache is now invalidated when a run-state write settles TIMEOUT
  or SUPERSEDED, restoring the issue-#45 behaviour of reading the pump back
  rather than guessing.

- **`upload_schedule` no longer reports success for an enable write the pump
  never took.** The schedule-enabled flag lives in a part of the overview that
  no layer readback carries, so the confirm step could not see it and the settle
  event echoed the *request*. An upload whose enable leg was dropped settled
  `accepted` with the weekly program still off. The flag is now read back and
  the verdict accounts for it: a mismatch settles rejected (or partial when the
  layers did land), and an unreadable state settles timeout. Relatedly,
  `set_state_async` no longer writes the requested value into the cached
  schedule state — it ran whether or not the write was acknowledged, so a retry
  of a failed enable saw "already in the requested state", skipped the write and
  still settled accepted, and the reported `schedule_hash` agreed with it.

- **The demand detector no longer reads a frozen pump reading as a stopped
  pump.** `alpha_hwr` keeps the last published value when the BLE link drops
  rather than publishing NaN, so a motor speed of 0 RPM stayed a perfectly valid
  reading indefinitely and the detector kept selecting its pump-off branch —
  scoring the pump's own recirculation as household demand at full confidence.
  Each motor channel is now bounded by its own reporting age (separately, since
  speed outranks current in the pump-state decision), and a frozen channel
  selects the pump-on branch instead of asserting a confirmed stop. The pump-on
  branch has its own staleness bound, so it declines and reports
  `pump_on_uncertain`, which is the safe answer.

- **A continuation fragment beginning `0x24` or `0x27` no longer destroys the
  frame being reassembled.** Those bytes are ordinary payload mid-frame, but the
  transport treated any fragment starting with one as a new packet, discarding
  the frame in flight and dispatching a runt. Measured against the reference
  captures: 8 occurrences in 17,545 dispatches, roughly 0.02% frame loss, masked
  because the runt then failed CRC. Reassembly now only restarts when not
  mid-frame, with a one-second staleness guard so a truncated frame cannot wedge
  it.

- **`parse_frame` no longer underflows `payload_len`.** A Class 2/3 frame
  clamped to seven bytes computed `len - 8` and wrapped it to `SIZE_MAX` while
  still reporting the frame valid, and a frame declaring a total below the
  protocol's eight-byte minimum was reported valid with no class byte. Both are
  rejected now, and `tests/test_frame_parser.cpp` covers malformed input, which
  nothing did before.

- **The single-event slot search no longer guesses.** With a cold cache
  `find_free_single_event_slot()` answered slot 0 — a real slot — rather than
  its own "none available" result, so a caller that wrote to it could overwrite
  a live event or an active vacation. It now returns -1 and the caller warms the
  cache first. `refresh_single_events` also gained the overview precondition its
  sibling operations already had; without it the scan assumed 35 slots and spent
  a three-second timeout on each one a smaller pump does not have.

- **Schedule read-back sensors are change-gated.** The five layer sensors and
  the schedule hash were republished on every write settle and every reconnect,
  costing six API frames per subscriber each time for values that rarely move
  (issue #127). Bench-verified: three consecutive `refresh_schedule` calls now
  produce zero republishes.

- README dropped a stale reference to a local, gitignored hardware config that
  is not part of the repo.

- **`packages/README.md` contained a real pump's BLE MAC address** — the Quick
  Start block used an actual device MAC rather than the `AA:BB:CC:DD:EE:FF`
  placeholder every other example uses. Replaced.

- **`packages/README.md` documented sensors that do not exist and a lambda that
  would not compile** — it listed "Grid Voltage" and "Converter Temperature"
  (the real sensors are `AC Voltage`, `DC Voltage`, `PCB Temperature` and
  `Control Box Temperature`), and its m³/h→GPM example called
  `id(flow_rate)`, an ID no package assigns. Replaced with a `platform: copy`
  conversion that declares the `id` it uses. It also covered only 2 of the 6
  packages; the other four and two of the four root examples are now listed.

- **README and `docs/schedule-management.md` used a `text_sensor.` entity-domain
  prefix that does not exist in Home Assistant** — the schedule-card examples
  and the related-entities table showed
  `text_sensor.hwr_pump_schedule_layer_0` and
  `text_sensor.hwr_pump_single_events`. ESPHome text sensors surface under the
  `sensor` domain, which is what the card actually derives, so anyone copying
  those overrides pointed the card at entity IDs that match nothing. Corrected
  to `sensor.`.

- **Corrected the subtraction's quiet-baseline figures**, which did not match
  the bench source they were drawn from. The 0.15.0 entry and the
  `dhw_demand_logic.h` comments claimed the no-draw offset was
  `−0.04 ± 0.06 GPM across seven measurements` at `2000–3600 rpm`, with a
  `30:1` margin. The bench report gives `−0.10 ± 0.06 GPM` across **four**
  measurements (2400, 2400, 3000, 3600 rpm) and a signal-to-noise ratio of
  **25:1**; the "seven" appears to have been borrowed from the unrelated
  seven-run no-draw control group used for the `dhw_in_use` tier. The argument
  is unaffected and slightly strengthened — a more negative quiet baseline
  pushes error further toward false negatives — but the numbers were wrong.
  Comments only; no behavior change.

- **README brought current with v0.15.0** — documents the card's optional
  `forecast_entity` / `desired_entity` overlays, the `dhw_in_use` detector input
  and its `dhw_in_use_min_seconds` guard, the `upload_schedule` / `set_vacation`
  / `clear_vacation` services, and `set_pump_state`. All were already
  implemented and covered in `docs/`; only the README summary lagged.


- **The two coupled switches settle like the service they mirror** (issue #302).
  Toggling **Engage Pump** or **Schedule Enabled** now fires exactly one
  terminal `set_pump_state` `write_settled` event, with `origin: "entity"` and
  the empty `op_id` entity writes carry — including when there is nothing to
  write, and including when the pump is not synchronized and the toggle is
  refused.

  What a toggle emitted before depended on the state the pump was already in,
  which is what the client was writing to establish. Engage Pump ON showed all
  three outcomes on its own: one `set_pump_enabled` from Off, one
  `set_schedule_enabled` from Scheduled, and **nothing whatsoever** when the
  pump already matched. A client waiting for a result therefore had to know the
  answer in advance, and in the third case waited until it timed out. Two
  writes could also appear where one normally does, since an invalid cache
  forces both flags to be written.

  The refusal path was silent for the same reason: `set_engage_pump()` and
  `set_schedule()` returned without a word when the pump was not yet
  synchronized, where the identical write submitted through `set_pump_state`
  settles `rejected`.

  The fix is a deletion as much as an addition. `apply_pump_schedule_target_()`
  held its own copy of the diff-and-order logic; it now delegates to
  `submit_set_pump_state()` — the same composition, ordering and worst-leg
  aggregation the service has used since issue #92 — and reports the aggregate
  under `origin: "entity"`. The raw flag writes still surface as their own
  events underneath, exactly as they do under the service, and the terminal one
  arrives last. The autonomous dead-schedule repair (issue #124) settles the
  same way, under its existing `op_id: "auto:dead-schedule-repair"`.

  Reported from the end-to-end harness work in issue #63, which is the first
  client to have waited on an entity write and noticed nothing arrived.


- **Every other entity write settles too** (issue #305, split out of #302).
  The two coupled switches were the half issue #302 named; every remaining
  entity setter had the same hole. Each guards on `check_ready()` and returned
  in silence, so a setpoint moved on a dashboard, a mode selected, Remote Mode
  toggled, or a schedule entry saved while the pump was still synchronizing
  produced a log line and no `write_settled` event — where the identical write
  submitted through a service settles `rejected`. That window is every node for
  the first seconds after boot, and again after each reconnect.

  Fourteen call sites now say it: start/stop, the mode select, both remote
  toggles, all five setpoint numbers, the temperature range, both cycle-time
  controls, the flow limiter, and the three schedule-entry writes. The refusal
  carries the command it was refused under, `origin: "entity"`, and the
  `requested_*` echo where one exists — the five setpoint entities share the
  `set_setpoint` command, so without the echoed mode a client could not tell
  which control had been refused. No settled value field is populated: nothing
  was written and nothing was read back, so the event names no pump state.

  **Two statuses, kept apart.** Not-ready settles `rejected` and is worth
  retrying once the link is up. A refusal no reconnect could fix settles
  `invalid` — an unrecognised day name on a schedule clear, and a limiter write
  made before the limiter record has ever been read, where the enable flag
  would be a guess (issue #299). That is the distinction a client retries on,
  and the same rule the api bridge already applies to a malformed argument.

  `sync_pump_clock()` is deliberately unchanged: it returns false without
  calling `done`, its callers use the return value, and conflating "we did not
  try" with "we tried and the pump did not confirm" is a 10-second retry versus
  a 15-minute one. Nothing outside waits on it.

- **The `format_hex_pretty` include now spans the supported core range**
  (issue #306). Its `std::string` overloads moved from `esphome/core/helpers.h`
  to `esphome/core/alloc_helpers.h` in ESPHome 2026.5.0, and the forwarding
  shim left behind in `helpers.h` documents its own removal *before 2026.11.0*.
  CI installs the latest core on every run, so the old includes would have
  stopped compiling on whatever unrelated PR happened to be open that week.

  Switching to the new header outright would have broken the other end:
  `alloc_helpers.h` does not exist before 2026.5.0, and `packages/README.md`
  declares a floor of ESPHome 2024.6.0. So this is a version guard, in the same
  idiom `alpha_hwr.cpp` already uses for the `get_build_time_string()` split at
  2026.1.0, kept in one header (`components/alpha_hwr/alloc_compat.h`) rather
  than repeated at each call site.

  Three call sites, one of which was never an include: `control_service.cpp`
  reached `format_hex_pretty` only through `log.h` pulling in the string
  helpers, which `log.h` does not promise and is one reshuffle away from
  ceasing to do.

  No behaviour change on any core version — this is which header declares a
  function the component already called.

## [0.15.0] - 2026-07-30

### Added

- **`dhw_demand`: the heater's DHW in-use flag can declare a pump-on draw, after
  a guard** (issue #138) — a third tier now sits below continuation and the
  subtraction. If `dhw_in_use` has been *continuously* high for
  `dhw_in_use_min_seconds` (new key, default 70), it declares demand at
  confidence 0.6. It casts no vote, cannot displace a stronger tier, and only
  ever adds demand.

  The flag is unusable bare: measured over 2026-07-21→28 it fires ~77 times a
  day with a median duration of 15 s, and 89.7 % of its events are at or under
  70 s. The guard is the whole tier. What justifies trusting the survivors is
  corroboration from a channel sharing no sensor with the flag — on pump-on runs
  ≥60 s the lower tank falls a median **−0.390 °F/min** when it fires (n=26)
  against **−0.043 °F/min** when it stays silent (n=7). A 9× gap, and the right
  shape: cold makeup water entering the tank, not recirculation's slow bleed.
  The tier is finding real draws in the one regime where loop flow blinds the
  meter — worth ~20 minutes a week of otherwise-invisible draws, across 9
  windows totalling 121 of 60 480 cells.

  The guard is a new `SustainedHigh` tracker in `dhw_demand_logic.h`, mirroring
  Python's `_dhw_in_use_sustained` including the part that looks like a bug and
  is not: **a NaN sample breaks the run exactly as a low one does**, so a BLE
  dropout resets the timer rather than holding the last value. Python re-derives
  the run from a rolling window each tick, where a missing sample *is* a break;
  hold-last-value would be the more forgiving choice and would diverge. The
  tracker ticks in **both** pump branches, because the run has to be free to
  start while the pump is still off — otherwise the guard would silently cost
  another 70 s after every pump start.

  Intensity comes from the subtraction where it is available,
  `min(1, demand_gpm / 2.5)`, and otherwise from the shared no-claim constant
  `0.4` — **never from raw meter flow**. That was issue #143: scaling intensity
  off pump-on meter flow inverts the ordering, since a quiet 2.2 GPM loop would
  publish 0.88 while a real 0.25 GPM draw publishes 0.4.

  The pump-off `apply_dhw_in_use_boost` gate is unchanged. Boosting confidence
  on a bare flag read and declaring demand on a flag that has held for 70 s are
  different acts. `detection_method` gains `deterministic_dhw_in_use`.

  **On the record, two caveats.** The control group is seven no-draw pump-on
  runs over one week — a large, one-directional effect, but a week and not a
  season. And the measurement #138 named as the one that would actually settle
  the reopened question — how many of those pump-on cells the subtraction
  already catches — has **not** been run; the tier lands on the strength of
  being strictly additive rather than on knowing its marginal contribution.
  Separately, the same flag is a bad *oracle* and a useful *input*: scored
  against physics, 988 of its positive cells had no water behind them, which is
  why it is scored nowhere. Those two roles are easy to conflate and the answers
  are opposite.

- **Schedule card v6: optional forecast and desired-schedule overlays** — the
  grid showed what the pump is programmed to do but never why, so there was no
  way to see whether a pre-heat burst actually lands in front of predicted
  demand. `forecast_entity` paints a weekly forecast's demand windows as a
  translucent heat strip behind each day row, opacity scaled by peak
  probability; `desired_entity` outlines intervals the scheduler wants but the
  device is not holding, putting scheduler-vs-device drift where someone would
  act on it. Both default to null, so every existing card config renders
  exactly as before, and both are `pointer-events: none` beneath the
  interactive blocks — dragging and editing are untouched.

### Changed

- **BREAKING — `dhw_demand` measures pump-on demand instead of voting on it**
  (issue #149) — the pump-on branch counted five hydraulic votes: an inlet
  pressure transient, an absolute inlet-pressure floor, pump-side flow collapse,
  a motor-current spike and a power spike, plus a firmware-only head-rate vote
  riding on top. Controlled measurement showed that tier could not be made
  correct at any threshold. The two absolute votes were scalars on quantities
  that move with pump speed — no-draw inlet pressure runs 3.4 PSI at 1650 RPM to
  13.0 at 3600 while a real draw reads 5.7 at 2400, so the quiet case can sit
  6 PSI *below* the drawing case and every candidate value fails somewhere. The
  three derivative votes fired on the pump's own modulation: 73 % of their
  production firings over 29 days landed within 25 s of a self-initiated speed
  change. Together the tier contributed 33 cells in 30 days, statistically
  indistinguishable from the cells where nothing fired.

  It is replaced by a measurement. `flow − pump_flow` is household demand
  directly — the meter reads everything leaving the mains, the pump reports its
  own recirculation loop, and the difference is what the house drew. No RPM
  term, no fitted curve. On controlled runs with a human opening one tap, no
  draw at 2400–3600 RPM measures −0.10 ± 0.06 GPM across four measurements
  against +1.24 GPM with the tap open: non-overlapping, roughly 25:1, and the
  residual bias is *negative*, so a quiet loop clamps to zero and the error
  pushes toward false negatives rather than false positives. Because the pump
  reports 0 flow when stopped, the same expression collapses to the pump-off
  rule — one oracle for both regimes. Scored on the same 209 cells: precision
  0.530 → **0.808**, F1 0.677 → **0.849**, pump-on false positives 23 → **0**,
  with recall 0.936 → 0.894. The new tier is 17 TP / 0 FP on its own and
  recovers three cells the votes missed *mid-draw*, where inlet pressure had
  recovered above its floor while the tap was still open.

  Three guards carry it and all three are load-bearing.
  `pump_on_demand_flow_threshold` (0.3 GPM of *computed* demand) shares
  `flow_threshold`'s value deliberately, so both pump regimes agree on what
  counts as flow. `pump_on_demand_max_stale_seconds` (30) and
  `flow_max_stale_seconds` (60) bound each side of the difference
  separately — a difference is only meaningful if both readings are current, and
  the channels do not report alike: the pump every 10 s, the meter on change at
  a median 28 s. Gaps in the pump channel beyond 20 s are 1.0 % of gaps but
  **14.1 % of pump running time**, and during one bench run a reading that
  looked live was 90 s old, from before the tap was opened. ESPHome carries no
  provenance on a sensor value, so each channel now stamps its own last-update
  register from a state callback. `pump_on_demand_min_speed_rpm` (1950) exists
  because the pump *estimates* its loop flow rather than metering it, and near
  the bottom of its range the estimate reads low, so the difference goes
  spuriously positive with the tap shut (+0.45 GPM at 1650, +0.27 at 1800,
  ≈0 from 2001 up). Without it that bias produced 20 false positives of its own.
  1950 admits the whole production range — the pump's own 29-day minimum was
  1971 RPM — and excludes only bench-commanded speeds.

  `demand_level` on the pump-on branch now scales off the measurement,
  `min(1, demand_gpm / 2.5)`, retiring the vote-count formula
  `0.3 + 0.15 × (votes − 1)`, which has no meaning once there are no votes.
  Confidence rises with margin over the threshold,
  `min(0.90, 0.60 + 0.30 × margin)`, rather than with a vote count.

  The head-rate vote went too. It was gated on at least one other vote having
  fired, so with the shared votes gone it could never be reached; the Python
  detector had already deprecated its head channel, so "add it there" was
  closed. The 15 s startup-transient suppression window went with the derivative
  votes it gated — dead config that looks live is a trap.

  **Migration: delete nine keys.** `inlet_pressure`, `pump_power`,
  `pump_head_rate`, `inlet_pressure_transient_threshold`,
  `inlet_pressure_demand_floor`, `pump_flow_collapse_threshold`,
  `motor_current_spike_threshold`, `pump_power_spike_threshold` and
  `pump_head_rate_threshold`. A config that still sets one **fails at
  `esphome config` time** with a message naming what replaced it and why —
  deliberately, rather than being accepted and ignored, so it breaks while the
  person who typed it is still looking. Make sure `pump_flow` and `motor_speed`
  *are* wired: they are what pump-on detection now runs on. Without them the
  detector still works while the pump is off and reports `pump_on_uncertain`
  whenever it is running. `detection_method` gains
  `deterministic_pump_on_subtraction` and loses `deterministic_pump_on`.

  **On the record:** no pump-on rule may key off raw meter flow, and that is
  settled rather than merely untried (issue #138). Pump-on runs with no draw
  read a median 1.31 GPM (p90 2.22) against 1.74 (p90 2.27) with a draw — the
  distributions overlap almost entirely, so no threshold exists. The
  "~2.2 GPM recirculation baseline" both this repo and the companion detector
  quoted for years was the p90 of the no-draw case, not a baseline. Separately,
  the 1950 RPM floor is a property of the ALPHA's own flow estimator rather than
  of one plumbing installation, so it should transfer to other installs — but it
  was measured at one, which is worth stating rather than implying.

  This closes #143, #145, #146 and #132 as well: all four are defects in the
  vote tier, and retuning a design that should not exist is effort spent in the
  wrong place.

- **The `dhw_demand` pump-on tier ordering is now host-testable** (issue #144) —
  `dhw_demand_logic.h` states its own contract at the top: pure decision logic,
  nothing hand-mirrored into the test. The individual predicates honoured it,
  but the *ordering* did not — it lived inline in `DhwDemandComponent::update()`,
  so nothing under `make test` asserted that continuation is tried before the
  hydraulic votes, or that `pump_on_uncertain` is the fallback of last resort.
  Composition covered only by reading the `.cpp` is precisely the failure mode
  that let a stale `3.0f` head-rate threshold survive the units audit (#120) and
  fed `pump_off_flow_onset_is_confirmed` an argument that did not mean what its
  contract said, for months (#147/#148). The branch is now
  `decide_pump_on(PumpOnInputs, PumpOnVoteThresholds) -> PumpOnResult` in the
  header, with the continuation predicate extracted alongside it; `update()`
  reads sensors, resolves the startup-suppression window, and calls it. No
  behaviour change — the tiers, thresholds, confidences and published
  `demand_level` are identical, and the pump-off branch is untouched. 28 new
  assertions pin the ordering.

### Removed

- **BREAKING — `packages/alpha_hwr_pairing.yaml` no longer assigns
  `id: pump_head_rate_sensor`** — the id existed for exactly one consumer,
  `dhw_demand`'s `pump_head_rate:` key, which wired the head-rate vote. That vote
  was gated on `votes >= 1` and became unreachable the moment the five shared
  votes retired, so issue #149 removed it and the config key with it. Nothing has
  referenced the id since.

  **The `head_rate` entity is unchanged.** The pump publishes head rate natively
  over BLE, it is useful diagnostic telemetry, and it is still exported as "Head
  Pressure Rate" with the same `entity_category: diagnostic`. Only the id is
  gone — "the head-rate *vote* was retired" is not "stop reporting head rate".

  **Migration, one line:** if your own lambda or automation resolves
  `pump_head_rate_sensor`, re-add `id: pump_head_rate_sensor` under `head_rate:`
  in your own `alpha_hwr:` block. That is where a config wanting a handle on this
  sensor should declare it anyway, rather than depending on an id the package
  happens to assign.

### Fixed

- **The flow-onset debounce is no longer a no-op at the pump-off transition**
  (issue #147) — `pump_off_flow_onset_is_confirmed` exists to stop
  recirculation flow carried over from the pump-on state being read as a draw
  on its first tick, and it was fed a `prev_flow_present` computed with no
  regard for what the pump was doing on that previous tick. Since
  `prev_flow_ = flow` is assigned unconditionally every tick, and the meter
  reads the loop at 1.3–2.3 GPM against a 0.3 GPM threshold while the pump
  runs, the debounce was already satisfied at the instant the pump stopped —
  passing exactly where it was meant to bite.

  The previous tick is now qualified with `prev_pump_confirmed_off_`, which
  already existed for continuation detection and costs no new state.
  `prev_pump_confirmed_off` rather than a bare `!pump_on`, so a NaN-gap tick
  whose last-known state was ON cannot qualify. The composition moved into
  `dhw_demand_logic.h` as `prev_tick_confirms_flow_onset`, because the
  component is not host-testable (#144) and this logic had already drifted
  once: the host test asserted the correct behaviour until #120 found the
  mirror disagreed with production and aligned the test *down*. Production is
  brought up instead and the original assertion restored.

  Effect: flow present across a pump-off transition is held one tick before it
  may declare demand on flow alone, so a real draw still running when the pump
  stops is declared 10 s later than before. Draws beginning while the pump is
  already off are unaffected. **This does not fix post-shutdown coast-down**,
  which outlasts two ticks — time-to-quiet is median 2 s, p90 189 s, max
  299 s — so #147 stays open.

- **Read-all schedule and single-event chains no longer report success when
  reads fail** (issue #136) — `read_entries_async(-1)` walked all five layers,
  logged a warning for any that failed, and then terminated with a hardcoded
  `on_complete(true, ...)`; `read_single_events_async` discarded its per-slot
  `success` flag entirely and set `single_events_cached_ = true` regardless.
  Callers treat that boolean as "the pump's schedule is now known", so an
  all-fail read made `refresh_schedule` settle **ACCEPTED** with a
  `write_settled` event saying the refresh worked, published the hash of an
  empty grid, and let `find_free_single_event_slot()` hand out slot 0 — able to
  overwrite a live event that was simply never read back, which is the clobber
  class issue #92 exists to prevent. Both chains now count failures explicitly
  and report success only when every layer (respectively every slot) read back;
  a partial single-event read leaves the previous cache and its cached flag
  untouched. Failure is not inferred from an empty result, since a layer or
  pump with no enabled entries legitimately reads back empty. Every consumer
  branch this makes reachable was already written — the paths were simply dead.

## [0.14.0] - 2026-07-28

### Added

- **Heap diagnostics in both packages** (issue #127) — **Free Heap**, **Largest
  Free Block**, **Min Free Heap**, **Heap Fragmentation** (60 s polling) and a
  **Reset Reason** text sensor, via ESPHome's `debug` component. The node
  rebooted on a heap exhaustion inside the API write path with nothing in Home
  Assistant history to see it coming or to correlate against afterwards — the
  static RAM figure the build prints doesn't capture runtime heap with the BLE
  stack up. `Min Free Heap` is the one that survives the reboot's own
  recovery, and `Reset Reason` distinguishes a panic from an OTA or a power cut.

- **`demand_release_seconds` option on `dhw_demand`** (default `30`) — demand is
  recomputed from scratch every tick, so an input dithering around its threshold
  used to chatter the demand binary sensor on and off. Rising edges still pass
  through immediately; falling edges are now held for this long past the last
  positive tick. Set to `0` for the previous raw per-tick behavior. Closes the
  "threshold jitter" gap in issue #120. While the hold is what is keeping demand
  on, `detection_method` reports a new `demand_release_hold` value rather than
  the branch's own conclusion — otherwise the sensor would read ON alongside
  `deterministic_idle` at 100 % confidence, which says the opposite.
- **`demand_level` output sensor on `dhw_demand`** (issue #125) — estimated draw
  intensity, 0.0–1.0. The value was already computed on every tick and threaded
  through the publish path, but nothing read it; the only consumer was a verbose
  log line. It is part of the RFC-006 detector contract and the Python detector
  publishes the same field, so its absence was a contract gap rather than an
  unused local. Publishing it also required reconciling the pump-on branch, which
  emitted a flat `0.3` regardless of vote count against Python's
  `0.3 + 0.15 × (signal_count − 1)` (`detection.py:732`) — that scaling now
  matches. The vote count behind it is the five signals shared with Python, not
  all six: this firmware's head-pressure rate spike is deliberately firmware-only
  and has no Python equivalent, so counting it would have put a +0.15 offset and
  an otherwise unreachable 1.0 on a field the contract says both detectors
  publish identically. It still sharpens `confidence`, which is each detector's
  read on its own evidence and was never claimed to match.
  While `demand_release_seconds` is latching, the last live intensity is
  republished rather than 0.0, for the same reason `detection_method` reports
  `demand_release_hold`: a 0.0 intensity alongside an ON demand sensor states the
  opposite of what the sensor says.
- **`dhw_demand` documentation** — a
  [DHW Demand Detection](docs/architecture.md#dhw-demand-detection) section
  covering the two-branch design, the signal set and the startup guard, and a
  full [dhw_demand Component](docs/configuration.md#dhw_demand-component) option
  reference for the sensors and all thirteen thresholds.
- **Test coverage for the remaining `dhw_demand` gaps** (issue #120): NaN on each
  of the six pump-on inputs in turn (BLE dropouts must not crash or vote),
  sustained multi-tick draws with session-duration accrual across brief lulls,
  and threshold jitter not chattering the output.
- **`internal` write origin** — the `write_settled` event's `origin` field can
  now read `internal` alongside `service` and `entity`, marking a write the
  component made on its own (currently only the dead-schedule repair, issue
  #124). Clients that switch on `origin` should treat an unknown value as
  not-mine rather than assuming `service`.
- **`Component Build` entity** (issue #124) — identifies the installed *build*,
  not just the release: `git describe` of the component's source tree (resolved
  at codegen — ESPHome clones `github://` sources with git, and a `type: local`
  source is your working tree) plus the firmware build timestamp, e.g.
  `v0.13.0-30-g066640c-dirty (built 2026-07-27 20:08:58 -0700)`. `Component
  Version` reports the release version and so is identical across every build
  between two releases, which made it impossible to pin a behavior change to an
  install from Home Assistant history — how a three-day outage went unattributed.
  Degrades to `unknown` when git is unavailable (tarball install); the build
  timestamp still separates installs. Optional (`component_build:` on
  `alpha_hwr:`) and included in both packages; no subprocess runs unless it is
  configured. See [docs/configuration.md](docs/configuration.md#build-identification).
- **`Pump Run State` and `Schedule Stalled` entities** (issue #124) — the run
  state as `off` / `engaged` / `scheduled` / `stalled`, plus a diagnostic
  `problem` binary sensor that alarms on `stalled`. `Pump Run State` is the only
  entity that distinguishes `AUTO` from `STOP` while the schedule is enabled,
  since `Engage Pump` reads off for both. Both are optional entries on the
  `alpha_hwr:` component and are included in `packages/alpha_hwr_pairing.yaml`.
- **`pump_set_state` service** — a coupled run-state + schedule selector for
  automations, the programmatic sibling of the `Engage Pump` / `Schedule
  Enabled` switches. Takes `state: off | engaged | scheduled` and reaches that
  legal state in one call: `off` → `STOP`; `engaged` → `AUTO` + schedule off
  (continuous); `scheduled` → `AUTO` + schedule on (gated, never the dead
  `STOP`+schedule combo). It writes only the flags that differ, and honors the
  one-terminal-event contract in the tricky cases — a no-op (already in the
  requested state) still fires its `write_settled` event, and a partial failure
  reports the *actual* end state (read back from the pump) with a non-accepted
  status. Composed from the raw `pump_set_enabled` + `set_schedule_enabled`
  writes, which remain as escape hatches for writing a single flag deliberately.
  Documented in [docs/programmatic-interface.md](docs/programmatic-interface.md#run-state-and-the-schedule).
- **Vacation editor controls** — `alpha_hwr_schedule_editor.yaml` now provides
  Lovelace helper entities for setting a vacation without calling the service
  by hand: four `number` inputs (**Vacation Start Month/Day**, **Vacation End
  Month/Day**) plus **Set Vacation** and **Clear Vacation** buttons. "Set
  Vacation" holds the pump off from 00:00 of the start day through 23:59 of the
  end day (current year, whole-day granularity) via `submit_set_vacation`;
  "Clear Vacation" calls `submit_clear_vacation`, which auto-resolves the active
  `Stop` slot. Entities are `internal: true`; documented in
  [docs/schedule-management.md](docs/schedule-management.md).

### Changed

- **DHW demand now latches briefly instead of following every tick.** With
  `demand_release_seconds` defaulting to `30`, the demand binary sensor holds for
  30 s after the last positive tick. Because the session tracker sees the held
  value, the effective end-of-session delay becomes `demand_release_seconds` +
  `session_gap_tolerance_seconds` (30 + 60 s by default). Set
  `demand_release_seconds: 0` to restore the previous behavior exactly.
- **The head-pressure-rate vote stays firmware-only, permanently** (issue #120
  item 1). The companion Python detector deprecated its head channel outright
  rather than adding an equivalent, so the two implementations diverge here by
  design. This is safe because the vote is gated on at least one other signal
  having already fired, so it can only sharpen an existing detection, never
  create one. Rationale recorded in `dhw_demand_logic.h` and
  [docs/architecture.md](docs/architecture.md#dhw-demand-detection).
- **`dhw_demand` pure logic consolidated into `dhw_demand_logic.h`** (renamed
  from `dhw_demand_votes.h`, issue #120 item 2). It now also holds the pump-off
  flow-onset predicate, the demand release-hold and session accounting, none of
  which depend on ESPHome or `millis()` — anything time-dependent takes `now_ms`
  as a parameter, so the host tests drive it with injected timestamps. This
  removed the last hand-mirrored copy in the test suite, which had drifted: it
  gated flow onset on `prev_pump_confirmed_off`, a parameter production declared
  but never read, so the test asserted behavior the firmware did not have. The
  now-shared predicate matches production — flow present on the previous tick is
  the 2-tick debounce. Also dropped three dead parameters/locals from
  `detect_pump_off_` and added a NaN guard to the head-rate vote for symmetry
  with the other five.
- **"Pump Enabled" switch renamed to "Engage Pump", and the pump/schedule
  controls are now mutually exclusive** (matching the Grundfos GO app). Bench
  testing (motor RPM as ground truth) established that the pump runs only when
  `operation_mode == AUTO` **and** the schedule is off (continuous) or inside a
  window (scheduled) — so a `STOP` + schedule-enabled pump never runs, and the
  old independent switches let you sit in that dead state while "Pump Enabled"
  misleadingly read on. The two switches now model three states — **Off**
  (`STOP`), **Engaged** (`AUTO` + schedule off), **Scheduled** (`AUTO` +
  schedule on, gated to windows): turning on **Engage Pump** disables the
  schedule; turning on **Schedule Enabled** forces `AUTO` (so it can actually
  run) and the Engage Pump switch reads off; turning the schedule off stops the
  pump. "Engage Pump" reads derived state (`AUTO && schedule-off`) from the
  pump, so the two switches are mutually exclusive without optimistic faking. It
  engages the pump's *mode* (`operation_mode == AUTO`); whether the motor spins
  is mode-dependent — continuous in the constant modes, cycling in
  Temperature/Cycle-Time (see the `Pump Motor Active` sensor for the real motor
  state). The coupling lives in `AlphaHwr` (`set_engage_pump` / `set_schedule`)
  with pure target/display logic in `pump_schedule_ux.h` (host-tested in
  `tests/test_pump_schedule_ux.cpp`); the programmatic services
  (`pump_set_enabled`, `set_schedule_enabled`) stay raw and uncoupled.
  **Migration:** the switch's entity_id changes from `switch.<node>_pump_enabled`
  to `switch.<node>_engage_pump` — update any automations/dashboards that
  referenced it.
- **Lovelace schedule card (v5)** — `homeassistant/www/alpha-hwr-schedule-card.js`.
  v4 moved it off the removed aggregate Weekly Schedule JSON sensor to the
  per-layer `schedule_layer_0..4` read-back sensors and the `Schedule Enabled`
  switch. v5 changes the **Enable/Disable Schedule button** to toggle the
  `Schedule Enabled` *switch entity* (coupled behavior) instead of calling the
  raw `set_schedule_enabled` service, so enabling the schedule from the card
  forces the pump to `AUTO` (never a dead `STOP`+schedule) and disabling stops
  it — matching the Engage-Pump reconciliation. Config is simplified: `device`
  is the only required option (layer/enabled/single-event entity IDs are derived
  from it and can be overridden); the grid edit/save path is unchanged.

### Fixed

- **`upload_schedule` now republishes the schedule display** (issue #133).
  `WriteCommand::UPLOAD_SCHEDULE` was missing from the component's write-result
  switch, so a bulk grid upload recomputed the canonical hash for the
  `write_settled` event but left `sensor.<device>_schedule_hash` — and the five
  per-layer read-back sensors — on their old values until something else forced
  a read-back. The upload is specified to recompute the hash sensor, and the
  sync model is "poll that sensor until it matches", so every
  grid change looked like a permanent sync failure: the scheduler timed out,
  retried three times per reconcile, and re-uploaded the same grid to an
  already-correctly-programmed pump. `in_sync` stuck false and `sync_failures`
  climbed for a healthy device — the same shape as issue #124, with the
  polarity flipped. Worse, changes landed invisibly, which is how a bad grid
  reached the pump unnoticed.

  The rule now lives in `result_republishes_schedule()`
  (`write_operation_service.h`) so the host test drives the production
  predicate. Uploads key on the post-op hash being present rather than on the
  terminal status: an upload is five independent layer writes, so a `PARTIAL`
  run has still moved the device grid, and the sensor must track the device
  rather than the verdict. That is the same condition the event payload uses,
  so the sensor and the event cannot disagree. Single-entry schedule writes
  stay gated on the terminal status, where a rejection does mean nothing moved.

  The `write_settled` event now also carries `schedule_hash` when an upload is
  rejected because *every* layer failed confirm (review feedback on #133).
  Those failures each ran a readback, which refreshes the cache from the pump,
  so the hash describes the device — the old written-or-skipped test reported
  an empty hash there, which is a rejection *after* the wire work rather than
  before it, leaving consumers no way to learn what the pump actually holds.
  It is still empty when the upload is rejected before the first layer.

- **Control entities no longer republish unchanged state every poll** (issue
  #127). ESPHome's `number` and `select` `publish_state()` fire their state
  callback unconditionally — unlike `switch`, which dedups — so the ten polled
  template controls in `alpha_hwr_controls.yaml` emitted an API state frame to
  *every* connected subscriber on every `update_interval` whether or not the
  value moved: measured on hardware at **2.79 state frames/s per subscriber**,
  essentially none of them carrying a change (`Pump Control Mode` alone ran at
  1/s, always `Temperature Control`). The component's own text sensors did the
  same on the poll cadence: the schedule-state poll fired its "state change"
  callback on every poll rather than on an actual transition, republishing six
  identical schedule text sensors (~450 bytes) every 10 s (0.60/s), and `Active
  Alarms`, `Active Warnings` and `Control Mode` republished unchanged strings
  with an INFO log line each. Together that was ~68 % of all state traffic.
  The polled lambdas now gate on change (`publish_number_if_changed()` /
  `publish_option_if_changed()`, `components/alpha_hwr/publish_gate.h`) and the
  C++ publishers announce transitions only, taking the steady-state floor to
  zero frames; `Pump Control Mode` also drops from a 1 s to a 5 s poll, matching
  the setpoints. Nothing is lost: these entities only re-read a cache the
  component already refreshes on its own (`control_state_poll_interval`, issue
  #54), and ESPHome sends every entity's current state on connect, so a
  reconnecting client still gets the full picture. This did not cause the
  `std::bad_alloc` reboot in
  `APIOverflowBuffer::enqueue_iov` — that abort is in ESPHome's API layer, with
  no `alpha_hwr` frames in the backtrace — but it was a permanent, avoidable
  load on exactly the buffer that overflowed, worst under the multi-subscriber
  DEBUG conditions of a bench session.
- **`dhw_demand` no longer republishes unchanged detector outputs every tick**
  (issue #129) — the same defect class as #127, in the component left out of that
  fix's scope. `sensor::publish_state()` and `text_sensor::publish_state()` notify
  the frontend unconditionally, so `DHW Detection Method`, `DHW Detection
  Confidence`, `DHW Demand Level` and `DHW Session Duration` each emitted an API
  state frame per subscriber on every 10 s tick: measured over a 495 s idle window
  at **0.40 frames/s per subscriber with not one distinct value among the 200
  publishes**, which after #127 was 23 % of all remaining state traffic on the
  node. `DHW Demand` was quiet throughout, because `BinarySensor::publish_state()`
  dedups internally — one tick publishing one deduped entity and four undeduped
  ones was an accident of which base class each output happened to use, not a
  design choice. The four now gate on change
  (`publish_sensor_if_changed()` / `publish_text_sensor_if_changed()`,
  `components/dhw_demand/publish_gate.h`), taking an idle window to four frames
  total. Nothing is lost: these are step-valued detector outputs, flat between
  transitions, and ESPHome sends every entity's current state on connect. A
  running draw is unaffected — `session_duration` moves every tick while a session
  is open, so only the idle `0 s` stream is silenced. The gate honours
  `force_update: true`, so a consumer that wants one of these as a per-tick
  heartbeat can still have it. The Python detector deliberately stays
  unconditional: its RFC-006 MQTT contract publishes all four fields in one
  message whose HA discovery config carries `expire_after: 30`, making cadence
  load-bearing for availability there, whereas over the native API availability
  follows the connection.
- **A stalled schedule (`STOP` + schedule enabled) is now detected, visible, and
  repaired** (issue #124, regression from the #121 reconciliation). That
  combination can never run — every window passes with the motor idle — and it
  was both permanent and invisible: the reconciliation ran only from command
  paths, so nothing reconciled run state at boot and the state survived every
  reboot, while `Engage Pump` reads off for *both* `AUTO` and `STOP` once the
  schedule is on, making a stalled pump byte-identical to a healthy scheduled one
  in Home Assistant (the pre-#121 switch tracked raw `operation_mode`, so the
  rename removed the last observable). A real installation sat stopped for three
  days with every health entity green. The check now runs with the periodic state
  poll — so it covers boot *and* out-of-band changes made by the Grundfos GO app
  or the raw services — and converges the pump to **Scheduled** (`AUTO` +
  schedule on), keeping the schedule intent rather than clearing it. A repair
  normally follows an external write that created the stall, so the component
  cannot spin on its own; as a backstop against a pump that reverts to `STOP` by
  itself, attempts are spaced at least five minutes apart (across stall episodes
  and reconnects alike). It is suppressed while a vacation (`Stop` single event)
  covers the current time, where a stopped pump is the commanded state. The
  repair reports itself with the new `origin: "internal"` and
  `op_id: "auto:dead-schedule-repair"` on its `write_settled` event, so an
  automation can distinguish the node repairing itself from a user action. To hold the pump off, turn
  the schedule off (`pump_set_state: off`) — stopping the pump under an enabled
  schedule is the stalled state, not a pause.
- **`dhw_demand` session close was off by one tick against the Python
  detector** (issue #125). `SessionTracker` closed a session once demand had
  been absent for `>= session_gap_tolerance_seconds`, while the Python
  `SessionAggregator` closes on strictly greater (`session.py:175`, `:197`). At
  exactly the tolerance the firmware closed and Python did not. Both now keep the
  session open at the boundary. Immaterial at a 10 s tick, but the two are meant
  to be the same rule.
- **`dhw_demand` read the clock twice per tick** (issue #125). `update()`
  captured `millis()` for the release-hold decision and `update_session_()` then
  called `millis()` again. The two ran microseconds apart so nothing misbehaved,
  but the hold and the session are deliberately coupled — the tracker sees the
  held value — so they now share one timestamp.
- **Single events (and vacations) never ran at the intended time — the
  `begin`/`end` timestamps were written as UTC, but the pump's clock is *local*
  Unix time.** The pump's RTC is local Unix time (GENI `unix_rtc`), while our
  service/`mktime` timestamps are UTC epoch, so every single event opened its
  window offset by the local UTC offset (~7 h in PDT) and the pump never fired
  it during the intended period. It still round-tripped byte-identically, so the
  write settled `accepted` and the slot looked correct — the bug was invisible
  to verification. `schedule_service` now shifts single-event timestamps to the
  pump's local-Unix domain on write and back to UTC on read (`utc_to_local_unix`
  / `local_unix_to_utc`); `SingleEvent` stays UTC everywhere else, so confirm,
  cache, and display are unchanged. Verified three ways: the GENI profile
  (`unix_rtc` is "local Unix time"), a bench A/B (a raw-UTC event never fires; an
  offset-corrected one runs at the right wall time), and the Grundfos app's own
  captured single-event bytes (`0x697dc634` = `timegm(09:07 wall)` = local-unix,
  byte-identical to what the fix now writes). This was the reason single-event /
  nowcast runs silently did nothing.
- Schedule layer writes (Obj `0xDE01`) no longer log a spurious `Command
  timeout waiting for Obj 56833 Sub 0` **warning**. These writes are
  fire-and-forget — the pump commits on timeout and its ACK arrives outside the
  response window — so the expected timeout is now logged at DEBUG via a new
  `quiet_timeout` transport flag. All other commands still warn on timeout,
  where it signals a real error; the timeout duration is unchanged so a late
  ACK can still be matched.
- **`dhw_demand`: the `dhw_in_use` confidence boost was applied regardless of
  pump state.** The upstream flag is only trustworthy while the recirculation
  pump is off — with the pump running it routinely latches high for a fixed
  ~60 s with no real draw behind it, so boosting a pump-on detection with it
  just added confidence to a phantom. The boost is now gated to pump-off,
  matching the corrected reference implementation.
- **`tests/test_dhw_demand_logic.cpp` had drifted from the component.** Its
  hand-mirrored `pump_head_rate_threshold` was still the pre-units-audit
  `3.0f` (kPa/s) while `dhw_demand.h` now uses `0.31f` (m/s), so the head-rate
  vote never fired in the test and the unguarded-startup case silently checked
  two votes instead of three. Synced the constant and corrected the
  expectation (0.65 → 0.80); added coverage for the boost gating above.
  Extracted the pump-on vote logic and threshold defaults into
  `dhw_demand_logic.h`, a header with no ESPHome dependency, so the test
  calls production code directly instead of hand-mirroring it — the class
  of drift above can't recur.

### Removed

- **`tools/check_dhw_demand.py`** (issue #120 item 4). The live-snapshot
  diagnostic held a fourth hand-copied set of detection thresholds with no test
  guarding it, reimplemented only two of the six pump-on votes with no
  derivatives or startup suppression, and queried a `text_sensor.` entity id in a
  domain Home Assistant does not have. Nothing in the repo referenced it. Use the
  component's own `confidence` and `detection_method` sensors instead —
  `detection_method` names the exact rule that fired on each tick.
- Removed the unused aggregate **Weekly Schedule** JSON text sensor
  (`schedule:` in `alpha_hwr_pairing.yaml`, `CONF_SCHEDULE`). It was the sole
  source of the `Schedule JSON truncated to 255 chars` warning — a full
  schedule overflowed HA's 255-char entity-state cap and truncated to invalid
  JSON. Nothing consumed it: the schedule editor reads the cache directly, and
  full-grid read-back is served by the per-layer `Schedule Layer 0..4` sensors
  plus `Schedule Hash`, which fit the cap by construction.

## [0.13.0] - 2026-07-23

### Added

- **Vacation scheduling** — new `set_vacation` (`"begin_ts,end_ts"`) and
  `clear_vacation` Home Assistant services put the pump into a multi-day
  pump-off period, matching the Grundfos Home app's vacation feature (which the
  GO app does not expose). A vacation is a `Stop`-action single-event
  (`ClockProgramSingleEvent`, Object 84) that overrides the weekly schedule for
  its range; bench-confirmed on hardware that it idles a running pump. The
  single-event write path now carries the action byte (`Auto` = one-time run,
  `Stop` = vacation) instead of hardcoding `Auto`; `clear_vacation`
  auto-resolves the active `Stop` slot. New **Vacation** text sensor shows the
  active range, and the Single Events display now labels each event `(run)` or
  `(off)`. Host tests cover the `Stop` write and the clear-by-action resolution.

### Fixed

- **Schedules showed as "pump will be idle" in the Grundfos app** — the
  component preserved the pump's existing `ClockProgramOverview.default_action`
  on every schedule write instead of setting it, unlike the Grundfos app, which
  always writes `default_action = Stop`. When the pump's `default_action` was
  `Auto` (0x02), the app rendered the whole schedule as *"pump will be idle"*
  even though the interval windows themselves were correct (`action = Auto`,
  i.e. run) — the source of the long-standing "inverted schedule" confusion.
  Schedule writes now explicitly set `default_action = Stop` (0x01) at all three
  overview-write paths (`set_state`, `set_state_async`,
  `send_configuration_commit`), matching the app. Bench-confirmed on hardware:
  forcing `Auto` reproduces the "idle" label, forcing `Stop` restores
  *"pump will run"*. No change to the interval action, the upload payload, or
  the canonical hash, so the external scheduler/RFC need no changes. The
  mislabeled `default_action = START` comment (0x01 is Stop) is corrected.

### Changed

- **Units audit — Head unified to meters, Confidence to percent.** A full
  audit cross-checked every entity's unit interpretation against the GENI
  reverse-engineering resources (reference decoders, `unit_index_mapping.csv` /
  `unit_factor_mapping.csv`, the device profile, and captured traffic). **No
  decode/scaling bugs were found** — live telemetry (flow m³/h, head m, temp °C,
  RPM, W, V, A, bar) and every setpoint/trend/statistic factor match ground
  truth. Two presentation inconsistencies were resolved:
  - The **Head** sensor now reports **meters of head** (the pump's native unit,
    matching the pressure setpoints) instead of kPa; **Head Rate** is now
    **m/s**. The Head sensor loses its `pressure` device_class (`m` is not a
    valid Home Assistant pressure unit). The DHW `pump_head_rate_threshold`
    default is rescaled `3.0` kPa/s → `0.31` m/s.
  - **DHW Detection Confidence** now reports a **percentage (0–100 %)** instead
    of a blank-unit 0–1 ratio.

    Home Assistant history for these three sensors steps at the upgrade (values
    differ from prior points by the constant conversion factor). No setpoint
    (the values users write) changed.

### Added

- **Units audit reference + regression test.** A new
  [`docs/units-audit.md`](docs/units-audit.md) catalogues every entity's
  unit, factor, decode site, and the resource that confirms it, with a
  re-verification procedure. A new host test `tests/test_telemetry_units.cpp`
  pins the physical-unit interpretation of every telemetry field (no
  GENI-scaling, no ×3600 on telemetry flow, no temperature offset) so a
  future reintroduction of the #88 class of bug fails the suite.
- **Node name in the settle event**
  ([#113](https://github.com/eman/esphome-alpha-hwr/issues/113)) —
  `write_settled` events now carry a `node` field (the controller's ESPHome
  node name, from `App.get_name()`), so deployments with more than one
  controller can attribute each event to its source. Unlike Home Assistant's
  `device_id` — opaque, and regenerated if the device is removed and re-added
  — the node name is stable, human-readable, and matches the service-call
  prefix. Present on every event, including empty-`op_id` entity writes.

## [0.12.0] - 2026-07-19

### Added

- **Cycle Time flow setpoint**
  ([#107](https://github.com/eman/esphome-alpha-hwr/issues/107)) —
  The flow the pump targets during cycle-mode ON periods (the Obj 91 Sub 421
  stored setpoint, previously settable only from the GO app) is now exposed:
  a `flow` argument on `pump_set_cycle_times` (m³/h, 0.1–10.0) and a new
  "Cycle Flow" number entity. All three cycle fields accept `0` =
  keep-existing, resolved from a mandatory fresh read of the pump's stored
  config, so flow-only and single-period writes are safe; a kept flow is
  still echoed to the pump byte-for-byte. The settle event gains `flow` and
  `requested_flow` fields.

- **Bulk schedule upload + sync hash** —
  New `upload_schedule` service uploads the entire 7×5 weekly grid in one
  call with full-state clear-and-set semantics; layers whose fresh readback
  already matches the desired image are skipped (a no-change re-upload costs
  zero BLE writes). New `partial` terminal status and `layers_written` /
  `layers_skipped` / `schedule_hash` event fields. New `schedule_hash` text
  sensor publishes a canonical FNV-1a-64 hash of the cached grid so external
  schedulers can verify sync without a full read-back; the pure payload/hash
  codec (`schedule_codec`) ships with host tests whose golden vectors are the
  cross-language contract for any scheduler mirroring the hash. Bench client
  gains an
  `upload` subcommand with local expected-hash computation. Five
  `schedule_layer_0..4` text sensors publish each layer's compact JSON for
  full-grid cold-start recovery, and auto-slot
  resolution reuses expired single-event slots so the 35-slot pool cannot
  exhaust.

- **Programmatic write-and-verify interface** (structural refactor,
  [#92](https://github.com/eman/esphome-alpha-hwr/issues/92)) —
  A new write-operation layer (`services::WriteOperationService`) owns every
  pump write: writes queue and run strictly one at a time, each builds its wire
  frames from the caller's arguments (never from a possibly-stale cache), each
  is confirmed by reading the settled value back from the pump, and each ends
  in exactly one terminal `esphome.alpha_hwr_write_settled` event
  (`accepted` / `clamped` / `rejected` / `timeout` / `superseded`, with the
  value the pump actually holds and a caller-supplied `op_id` for
  correlation). Clients no longer insert fixed delays or guess at internal
  readback timing.
- **Unfused Class 3 START/STOP for on/off** — `pump_set_enabled` (and the
  dashboard switch) now send the pump's dedicated Class 3 run-state commands
  (START `0x06` / STOP `0x05` as SET, command ids and ACK shapes
  bench-contributed by jfriend00 in
  [#92](https://github.com/eman/esphome-alpha-hwr/issues/92)) instead of the
  fused 0x0601 control object. On/off now carries no mode and no setpoint at
  all — the last fused write leaves the on/off path — and a pump-rejected
  command settles `rejected` from the `[03 01]` descriptor nack. The run
  state is confirmed by readback as before.
- **Home Assistant services for pump control** — `pump_set_enabled`,
  `pump_set_mode` (unfused, via the object-86/sub-id-10 mode change from #98),
  `pump_set_setpoint`, `pump_set_temperature_range`, `pump_set_cycle_times`,
  registered in C++ (`api_bridge`). Requires `custom_services: true` and
  `homeassistant_services: true` on `api:` (set in all shipped packages and
  examples). Documented in `docs/programmatic-interface.md`.
- **Cycle-time write verification** — `set_cycle_times` gains the Object 91
  readback the legacy fire-and-forget setter never had: an unreadable DHW
  config before the write settles `rejected` with no write attempted, and a
  readback that keeps failing after the write settles `timeout`.
- **Settle-event refinements from the #92 contract review** — new `invalid`
  terminal status for malformed/out-of-range requests (deterministic,
  pre-wire; `rejected` now strictly means the pump or its state refused),
  new `origin` (`service`/`entity`) and `seq` (submission-order) event
  fields, and `requested_*` fields echoing the original request so the
  event is self-contained for logging and retries. Supersede keys for
  setpoint writes are now strictly per-mode: queued setpoints for different
  modes both run, since the pump stores an independent setpoint per mode
  (review catch).

### Fixed

- **All compiler warnings in the ESP-IDF build** — the component emitted 21
  warnings, all of them format mismatches. On ESP32, `uint32_t` is
  `long unsigned int`, so the `%u`/`%d` used for millisecond timers,
  timestamps, start counts and the BLE passkey did not match their arguments
  (`-Wformat=`); these are now `PRIu32` from `<cinttypes>`.
  `ESPTime::timestamp` is printed via a `long long` cast since `time_t` width
  varies by platform. The `"HH:MM"` helpers in `schedule_entry.h` tripped
  `-Wformat-truncation=` because the compiler could not prove a `uint8_t`
  hour/minute renders in two digits; the fields are now bounded so the 6-byte
  buffer can never truncate. No behavioural change — verified with a full
  ESP-IDF build (0 warnings) and the host test suite (`-Wall -Wextra`, 494
  assertions passing).

- **Cycle Time Control read/write targeted the wrong GENI object**
  ([#106](https://github.com/eman/esphome-alpha-hwr/issues/106)) —
  The component read and wrote cycle times through Object 91 Sub 430, which
  the GENI profile identifies as `TemperatureRangeControlUserSettings`
  (type 1012): its trailing bytes are min/max on/off-time LIMITS, invariant
  to the live configuration — which is why the entities always showed 5/15
  and writes never took. The live values are Object 91 **Sub 421**
  (`dhw_on_off_control_configuration_obj`, type 985: flow setpoint + on/off
  periods), confirmed byte-for-byte against GO-app captures. Cycle-time
  reads and writes now use Sub 421 with the GO app's exact frame shape,
  read-modify-write so the stored flow setpoint is echoed back verbatim,
  and no configuration commit (capture-verified as unnecessary). The Cycle
  Time ON/OFF entities are now wired to the pump's real cached values
  instead of optimistic hardcoded defaults, and the config is read at every
  cache sync.
- **Temperature-range writes zeroed the pump's on/off-time limits** —
  an adjacent consequence of the same misidentified struct: the Sub 430
  write's tail bytes (sent as constants) are the pump's limit fields.
  They are now echoed back from the last read instead of being overwritten.

### Changed

- **Constant Speed Setpoint slider steps by 25 RPM instead of 100**
  ([#108](https://github.com/eman/esphome-alpha-hwr/issues/108)) —
  The pump resolves 25 RPM increments (each one yields a distinct,
  progressively higher flow), whereas a single 100 RPM click moved flow by
  roughly 20-25%. The finer step also divides the 500 RPM floor evenly, so
  hardware minimums such as 1650 RPM can be selected directly rather than
  reached by undershooting and waiting for the pump to clamp. Range
  (500-4500 RPM) is unchanged.

- **`pump_set_cycle_times` surface changes for #107** — the service now
  declares a `flow` field; Home Assistant requires every declared field, so
  existing callers must add `flow: 0` (the bench tool defaults it
  automatically). Minute arguments of `0` now mean keep-existing (previously
  settled `invalid`). A cycle write the pump wholly ignores now settles
  `rejected` instead of `clamped`, matching setpoint semantics. The
  `requested_on_minutes`/`requested_off_minutes` event fields are emitted
  independently and omitted for kept fields. The Cycle Time ON/OFF entities
  now write only their own period (the other is keep-existing) instead of
  reconstructing it from cache with hard-coded fallbacks.
- **Entity writes route through the write-operation layer** — the dashboard
  number/switch/select lambdas now get the same serialization and verify
  readbacks as the services (their settle events carry `op_id: ""`). This
  closes the issue-#92 collision class for UI users too (e.g. a setpoint write
  followed immediately by off can no longer revert the setpoint), and entity
  callbacks now report the write's *terminal* result rather than
  fire-and-forget success.
- **Schedule services migrated from YAML to C++** with unchanged names and
  `data`-string formats plus a new optional `op_id` argument; the
  `api: services:` block was removed from
  `packages/alpha_hwr_schedule_editor.yaml`. **Behavior change:** schedule
  writes are now verified with a post-commit readback and can report
  `rejected`/`timeout` where they previously logged "OK" unconditionally;
  malformed `data` strings now settle as `rejected` instead of failing
  silently. `set_schedule_enabled` no longer falls back to a blind
  hardcoded-defaults overview write, and schedule-entry writes always
  fresh-read the layer first so out-of-band edits of other days can't be
  clobbered. Single-event slot auto-selection reads the slots before choosing,
  so it can no longer overwrite slot 0 on a cold cache.
- `ControlService` no longer owns multi-step write sequencing (its high-level
  start/stop/set-mode/setpoint setters moved into the operation layer); it
  keeps the pump-state cache, the #91 coordination guards, and the wire
  primitives.

## [0.11.0] - 2026-07-17

### Added

- **"Component Version" diagnostic entity** —
  A new diagnostic text sensor reports this ESPHome component's own release
  version (distinct from the pump-firmware Software/Hardware/BLE/Product Version
  entities). It is recorded in Home Assistant history, so the component version
  that produced any state is answerable after the fact and on bug reports. The
  value is kept in sync automatically by `tools/bump_version.sh`
  (fixes [#95](https://github.com/eman/esphome-alpha-hwr/issues/95)).

### Fixed

- **Constant Flow setpoint entity showed the requested value, not the stored one** —
  #82 added a 1.2s post-write readback so each setpoint entity reflects the value
  the pump actually stored (e.g. when it clamps or rejects the request). Constant
  Flow was the only setter still missing it — a leftover from when flow readback
  was thought unreliable, which #88/#90 later showed was a write-units bug (m³/h
  vs the pump's native m³/s). `set_constant_flow_async` now performs the same
  readback as the other three modes, so the Constant Flow entity settles on the
  pump's real stored/clamped value (fixes
  [#96](https://github.com/eman/esphome-alpha-hwr/issues/96)).

- **Cycle-time config could permanently block all commands** —
  `is_cache_valid()` — which gates `pump_ready` and every command (start/stop, mode
  change, setpoint writes, remote mode, schedule writes) — required the pump's
  cycle-time config (Object 91), but the parser only read those bytes when the
  payload was long enough. A short or unusual Object 91 payload could leave the
  cycle-time fields at their `-1` "unknown" sentinel, making the pump report
  not-ready forever with every command silently rejected. Cycle-time config is no
  longer part of readiness (it is not displayed anywhere; same rationale as
  excluding the mode-specific setpoints). Separately, the cycle-time bytes are now
  range-validated to 1–60 on read, so a `0xFF`/`0`/out-of-range value maps to
  "unknown" instead of truncating a `uint8_t` into the `int8_t` sentinel. Both bugs
  were latent — real pumps return valid data — so this is a robustness fix against
  short or unusual Object 91 payloads. Fixes
  [#94](https://github.com/eman/esphome-alpha-hwr/issues/94).
- **Uncoordinated `current_mode_` writes during mode switches** —
  A control-mode readback that landed after a mode command was issued but before
  the pump had applied it (most easily via the 30 s out-of-band poll, but also the
  post-command reconciles) reported the *old* mode and silently overwrote the
  optimistic new mode, which then drove `sync_cache_async` into an unbounded 2 s
  retry loop that NaN-ed the setpoint cache. `set_mode`/`start` now record the
  commanded mode as *pending*, and every readback writer (`get_mode_async`, passive
  notifications) keeps the commanded mode until a readback confirms it — so a
  stale/in-flight read can no longer corrupt the mode or flicker the setpoint to
  `unknown`. The sync retry is now bounded and, if the pump never applies a command,
  falls back to accepting the pump's reported mode instead of forcing the commanded
  one forever. Out-of-band change detection (issue #54) is preserved: with no
  command pending, a readback still adopts the pump's mode. (Addresses the
  coordination race in [#91](https://github.com/eman/esphome-alpha-hwr/issues/91);
  the setpoint-clobber symptom itself was already removed by the #97/#83 fix.)
- **Three of six control modes could not be entered, and mode changes clobbered
  the stored setpoint** —
  On v0.10.3, selecting Constant Pressure, Proportional Pressure, or Constant Flow
  did nothing and aborted after a 5-second timeout; Constant Speed only worked when
  it was already the active mode. The root cause was that mode changes were sent as
  a start/stop command (GENI `overall_operation_local_request`, wire Obj 0x0601)
  that also writes the mode's *setpoint* — so the component either read the pump
  first (a read that always timed out and aborted) or fell back to a default value
  that durably overwrote the pump's stored setpoint (~3671 for Constant Speed).

  Mode switching now uses the dedicated GENI control-mode object
  (`overall_control_mode_local_request`, object 86 / sub-id 10, wire Obj 0x0A01),
  exactly as the Grundfos GO app does. Per the pump's own GENI profile this object
  changes only the control mode and ignores the run state and setpoint (the payload
  carries `operation_mode = NoCmd` and `set_point = NaN`), so every mode is
  reachable regardless of cache state and each mode's stored setpoint is preserved.
  Confirmed on-device: switching into a mode with a cold cache now leaves its
  setpoint intact (previously it was overwritten with the default). This also
  inherently prevents a mode change from force-enabling the pump. Verified against
  BLE captures and the pump's GENI profile descriptor
  (fixes [#97](https://github.com/eman/esphome-alpha-hwr/issues/97), fixes
  [#83](https://github.com/eman/esphome-alpha-hwr/issues/83), reinforces
  [#45](https://github.com/eman/esphome-alpha-hwr/issues/45)).
- **Misleading mode-change log** —
  The mode dropdown logged `Set mode result: SUCCESS` ~5 seconds before the
  asynchronous command actually resolved. It now logs that the change was *queued*,
  not that the pump applied it (issue #97).

## [0.10.3] - 2026-07-16

## [0.10.2] - 2026-07-16

### Fixed

- **Constant Flow Setpoint scaling and readback** —
  Fixed a unit mismatch where the pump's native `m³/s` Constant Flow setpoint was being passed to Home Assistant as `m³/h` without conversion. This caused the readback to display as `0.000694 m³/h` instead of `2.5 m³/h`, and caused the pump to silently reject all writes because `2.5 m³/s` (`9000 m³/h`) was severely out of range. 
  (fixes [#88](https://github.com/eman/esphome-alpha-hwr/issues/88), fixes [#81](https://github.com/eman/esphome-alpha-hwr/issues/81), roots-causes [#44](https://github.com/eman/esphome-alpha-hwr/issues/44), PR #90).
- **Constant Flow abort bug** —
  Fixed an issue in `with_resolved_setpoint` where `CONSTANT_FLOW` changes would hard-abort instead of proceeding with the setpoint write.
- **Transport Sub-ID Matching bug** —
  Fixed `send_apdu_command` calls in the control service that were inadvertently passing the Object ID as the Sub ID due to reversed argument order.
- **Prevent NVM Clobbering** —
  Introduced `with_resolved_setpoint` to query the pump for its active setpoint if it isn't cached locally. This prevents `send_control_request` from falling back to hardcoded default suffixes that clobbered the pump's NVRAM when turning it on or changing modes (fixes [#83](https://github.com/eman/esphome-alpha-hwr/issues/83), PR #89).
## [0.10.1] - 2026-07-11

### Fixed

- **Event log read timeout** —
  Fixed an issue in `EventLogService` where the expected Object and Sub-ID fields were swapped for the event log read command, causing the transport layer to drop the response and time out.

## [0.10.0] - 2026-07-11

### Fixed

- **Missing entities on reconnect / startup race conditions** —
  Added `sync_cache_async` orchestration after the session is READY 
  which polls Object 86 and Object 91 before the pump is marked as "Ready". 
  This ensures that all configuration bounds and telemetry entities populate when 
  the pump connects, eliminating stale `0`/`NaN` gaps and the need for manual polling
  (issue [#67](https://github.com/eman/esphome-alpha-hwr/issues/67)).
- **Multi-parameter writes scrambled defaults due to missing cache** —
  Because `sync_cache_async` now guarantees internal telemetry bounds (like 
  AutoAdapt and Cycle Times) are populated before writes are permitted, 
  multi-parameter setters (e.g. Temperature Range or AutoAdapt toggle) no longer
  rely on uninitialized dummy defaults `0`. The setters reuse genuine pump 
  values for unchanged fields, preventing unintended resets or scrambled schedules
  (issue [#70](https://github.com/eman/esphome-alpha-hwr/issues/70)).
- **Clock Drift reports exact timezone offset (-25,200s)** —
  Fixed an issue where the C-library `mktime` fell back to UTC parsing when reading the pump's RTC. The integration now correctly requests `time_id` and leverages ESPHome's internal `ESPTime` engine to accurately interpret the pump's local timezone
  (issue [#76](https://github.com/eman/esphome-alpha-hwr/issues/76)).

## [0.9.0] - 2026-07-10

### Added

- **Host-based unit tests and GitHub Actions CI** —
  Added a mock ESPHome harness in `tests/mocks/` to allow protocol and service
  logic to be compiled and executed natively on the host using `g++` instead of
  requiring an ESP-IDF toolchain. Added test suites for the Transport FSM and
  ScheduleService payload generation. CI is now enforced via GitHub Actions on
  pushes and pull requests to `main` (PR #59).

### Fixed

- **Remote Mode lockout and spontaneous start** —
  Fixed `disable_remote_mode` to correctly send a `LOCAL` (0x08) command rather than a
  `START` (0x06) command, which caused the pump to spontaneously run when disabling remote mode.
  Also removed the automatic `enable_remote()` call from the `pump_enabled` switch, which was
  causing the pump to ignore all commands for ~40 seconds after being turned on
  (issue [#66](https://github.com/eman/esphome-alpha-hwr/issues/66)).

- **Temperature Range writes could report success without persisting pump values** —
  `ControlService::set_temperature_range_async()` queued the Object 91/Sub 430
  write as fire-and-forget while the preceding mode-switch control request
  still auto-scheduled an early commit. That commit could run before the
  Object 91 write completed, leaving the pump on its previous/default stored
  range while Home Assistant still reported success. Temperature range writes
  now (1) disable the step-1 auto-commit for multi-step setters, (2) wait
  through a response/timeout window before committing, and (3) trigger a
  readback so cache reflects the pump's persisted state
  (plus Class 10 short ACK dispatch for the Obj 91/Sub 430 write path, and
  no success callback when the write ACK is missing),
  (issue [#65](https://github.com/eman/esphome-alpha-hwr/issues/65)).
- **Changing Temperature Range Min/Max silently re-enabled AutoAdapt** —
  The `Temperature Range Min` and `Temperature Range Max` number entities
  hardcoded `autoadapt = true` in their `set_action` lambdas, so adjusting
  either bound always turned AutoAdapt back on even if the
  `Temperature AutoAdapt` switch had just been turned off. Both setters now
  use the pump's cached AutoAdapt state (`get_cached_autoadapt()`) when
  writing the temperature range, preserving the current switch state while
  updating range bounds
  (issue [#64](https://github.com/eman/esphome-alpha-hwr/issues/64)).

- **Remote Mode switch reflected local intent rather than actual pump state** —
  The switch was declared `optimistic: false` (implying it tracks the pump's
  real state) but was driven solely by `is_remote_mode_enabled_`, a flag set
  only when our own enable/disable commands received a clean ACK. The pump's
  `control_source` byte — present in the passive Control Mode Status
  notification (Obj 0x2F01 / Sub 0x0001, OpSpec 0x0E, sent automatically
  after authentication) and in the response to the explicit Object 86 / Sub 6
  read (triggered by the periodic control-state poll, issue #54) — was parsed
  and logged but never applied to the cached state, so the switch would show
  stale data after a reconnect, a panel reset, or any external tool
  taking/releasing remote control.
  `update_mode_from_notification()` and the `get_mode_async` read callback now
  update `is_remote_mode_enabled_` when `control_source` is a recognized value
  (`2` = Remote/Digital, `1` = Local/Panel, matching the Python reference
  implementation). Unknown values (e.g. `0`, seen before the opcode fix in
  PR #50) are ignored so a stale byte cannot incorrectly clear a state that
  was confirmed by a command ACK
  (issue [#53](https://github.com/eman/esphome-alpha-hwr/issues/53)).
- **Component does not detect out-of-band pump state changes** —
  If the pump changes state autonomously (e.g., internal schedule execution,
  manual button press on the pump, external app control), the component's
  cached state diverges from the pump's actual state until the component is
  rebooted. This affects multi-control setups where exclusive component control
  cannot be guaranteed. Fixed by implementing periodic control state polling
  (Option 1 from issue discussion): a configurable background `get_mode()` read
  at regular intervals (default 30 seconds) to detect and sync state changes.
  Polling interval is configurable via the `control_state_poll_interval`
  parameter and can be disabled by setting it to 0. Users who want exclusive
  control can disable polling; those sharing pump control benefit from faster
  divergence detection.
  (issue [#54](https://github.com/eman/esphome-alpha-hwr/issues/54)).
- **Pump Enabled switch lag after toggling** —
  The pump does not send unsolicited notifications after Class 3 START/STOP
  commands, so the non-optimistic `Pump Enabled` switch in Home Assistant
  won't update from the component's cache until the next periodic telemetry
  poll (~10 seconds). This creates a user-visible lag: users toggle the switch
  and don't see confirmation until several seconds later. Fixed by scheduling
  an explicit post-command `get_mode()` readback ~500ms after every start/stop
  (reporter bench-tested this timing on real hardware and confirmed it resolves
  the UI lag reliably). The readback is non-blocking and non-critical; if it
  fails the UI simply updates on the next periodic poll as before.
  (issue [#52](https://github.com/eman/esphome-alpha-hwr/issues/52)).
- **Cross-mode setpoint contamination caused wrong setpoint on mode switches** —
  `ControlService` used a single shared `cached_setpoint_` field across all
  scalar control modes. Switching from, say, Constant Speed (2000 RPM) to
  Constant Pressure meant that 2000 was briefly the "pressure" cache, and a
  subsequent `start()` would convert it to Pascals and send ~19.6 MPa to the
  pump. Replaced the shared field with four independent per-mode caches
  (`cached_pressure_setpoint_`, `cached_proportional_setpoint_`,
  `cached_speed_setpoint_`, `cached_flow_setpoint_`); each mode reads and
  writes only its own slot, so a value set in one mode can never contaminate
  another. The mode-transition NAN-clearing workaround in `start()` is no
  longer needed and has been removed. Public getters on `ControlService` and
  the `AlphaHwrComponent` facade, plus the four number-entity lambdas in
  `packages/alpha_hwr_controls.yaml`, updated accordingly
  (issue [#51](https://github.com/eman/esphome-alpha-hwr/issues/51),
  PR [#57](https://github.com/eman/esphome-alpha-hwr/pull/57)).

## [0.8.0] - 2026-07-08

### Fixed

- **Remote Mode command used the wrong GENIbus opcode and never actually took effect** —
  `enable_remote_mode()`/`disable_remote_mode()` sent Class 3 command
  `[0x03, 0xC1, ...]` (OpSpec 0xC1 = INFO). Bench-verified against a real
  pump (2026-07-08) that this opcode is always rejected with a
  "descriptor-only" ACK (`[03 01 AC]`), while OpSpec `0x81` (SET) reliably
  produces the clean success ACK (`[03 00]`) — confirmed by sending both
  variants (and both the `0xF8`/`0x0A` source-address theories) back-to-back
  and comparing the raw ACK bytes, ruling out the source address as a factor.
  Switched both commands to `0x81`. Also added `Transport` support for
  matching short Class 3 ACK responses (previously only Class 7/10 responses
  were dispatched to command callbacks; Class 3 ACKs as short as 8 bytes
  were rejected by the existing 12-byte minimum-length guard before any
  class-specific matching ran), gated so a queued Class 3/7 command can only
  be satisfied by a response of that same class (an unrelated Class 10
  telemetry notification arriving first can no longer be mistaken for the
  answer). `is_remote_mode_enabled_` is now only updated once the pump's ACK
  actually confirms success, instead of being set unconditionally right
  after sending the command
  (issue [#46](https://github.com/eman/esphome-alpha-hwr/issues/46)).
- **Setpoint and mode changes unexpectedly enabled the pump** —
  `ControlService::set_mode()` and every setpoint setter
  (`set_constant_pressure_async`, `set_constant_speed_async`,
  `set_constant_flow_async`, `set_proportional_pressure_async`,
  `set_temperature_range_async`, `set_cycle_time_control_async`) hardcoded
  the GENIbus control frame's start/stop flag to "start" regardless of the
  pump's actual on/off state, so writing a setpoint or switching modes while
  the pump was off silently turned it on. Since the frame fuses mode +
  setpoint + on/off into a single write, there's no way to omit the flag —
  these calls now send the pump's actual last-known enabled state via a new
  `ControlService::with_resolved_enabled_state()` helper, which reads it back
  from the pump first if not yet known. If that read-back also fails, the
  control request is aborted entirely rather than guessing an on/off state
  (guessing "enabled" risked the original bug; guessing "disabled" would
  risk sending an explicit stop to a pump that was actually running).
  (issue [#45](https://github.com/eman/esphome-alpha-hwr/issues/45),
  PR [#49](https://github.com/eman/esphome-alpha-hwr/pull/49)).
- **Constant Flow Setpoint displayed a value off by ~1000x** —
  bench-verified against the physical pump (2026-07-08) that Class 10
  Object 86/Sub 6 returns the exact same raw value (~0.000694 m³/h)
  regardless of the actual commanded flow setpoint (tested 0.2/2.0/8.0 m³/h,
  all identical); converted to the originally reported units, that's
  `0.003056 gal/min` — an exact match. Since this register is confirmed
  unreliable for Constant Flow specifically, `ControlService` no longer
  applies its readback to the cached setpoint for this mode; the display now
  reflects only the last client-commanded value (from
  `set_constant_flow_async()`), showing unavailable until a value is
  explicitly set rather than a definitely-wrong number. Other modes are
  unaffected — they still read their setpoint from the register as before.
  A diagnostic warning log (`check_flow_setpoint_scale()`, added alongside
  this fix) is kept in case a different pump/firmware revision behaves
  differently.
  (Enabling the pump in Constant Flow mode no longer forces a hardcoded
  ~3671 RPM either, as that shares the fix for #43.)
  (issue [#44](https://github.com/eman/esphome-alpha-hwr/issues/44),
  PR [#48](https://github.com/eman/esphome-alpha-hwr/pull/48)).
- **Pump Enabled ignored the configured setpoint, forcing a hardcoded ~3671 RPM** —
  `ControlService::start()` called `send_control_request()` with no setpoint,
  so `CLASS10_CONTROL_MAP`'s default suffix (which decodes to exactly `3671.0`)
  was always sent for Constant Pressure, Proportional Pressure, Constant Speed,
  and Constant Flow modes on every enable, regardless of any setpoint
  previously configured. `start()` now reuses the pump's cached setpoint
  (converting meters back to Pascals for the pressure modes) when no explicit
  mode override is requested and a setpoint has already been read for the
  current mode; falls back to the previous default-suffix behavior otherwise,
  including for DHW On/Off, Temperature Range, and AutoAdapt modes
  (issue [#43](https://github.com/eman/esphome-alpha-hwr/issues/43),
  PR [#47](https://github.com/eman/esphome-alpha-hwr/pull/47)).
- **Memory leak in schedule "read all layers" async chain** —
  `ScheduleService::read_entries_async(-1, ...)` drove its layer-by-layer read
  loop with a self-referential `std::shared_ptr<std::function>`. `Transport::reset()`
  (called on every BLE disconnect) clears the pending command queue without
  invoking callbacks, so a disconnect landing mid-read left the self-reference
  unbroken and leaked the closure (cached entries, completion callback, and all).
  Rewritten using a stateless recursive lambda plus a plain, non-cyclic state
  object, so an interrupted read now cleans up normally regardless of when the
  disconnect happens (issue [#32](https://github.com/eman/esphome-alpha-hwr/issues/32),
  PR [#38](https://github.com/eman/esphome-alpha-hwr/pull/38)).

### Changed

- **Centralized GENI frame construction** — added `Transport::send_apdu_command()`;
  services no longer manually call `build_geni_packet()` and hardcode the
  transport-level Service ID/Source Address themselves, keeping protocol framing
  out of the services layer per the project's layered architecture
  (issue [#28](https://github.com/eman/esphome-alpha-hwr/issues/28),
  PR [#36](https://github.com/eman/esphome-alpha-hwr/pull/36)).
- **Schedule JSON generation moved into `ScheduleService`** —
  `ScheduleService::generate_json()` now owns building the compact JSON used by
  the Weekly Schedule text sensor; `AlphaHwrComponent` no longer reaches into
  `ScheduleService`'s internal cache directly
  (issue [#29](https://github.com/eman/esphome-alpha-hwr/issues/29),
  PR [#36](https://github.com/eman/esphome-alpha-hwr/pull/36)).
- **Consolidated schedule write encoding** — `write_entries()` and
  `write_entries_async()` now share a `build_schedule_apdu()` helper instead of
  duplicating the per-day entry encoding logic
  (issue [#30](https://github.com/eman/esphome-alpha-hwr/issues/30),
  PR [#41](https://github.com/eman/esphome-alpha-hwr/pull/41)).
- **Reduced `ScheduleEntry` memory footprint** — replaced the `std::string day_`
  field with a compact `uint8_t` day index (falling back to a sentinel for
  unrecognized day names instead of silently treating them as valid), and
  replaced a `std::map`-based per-day/layer entry count in schedule validation
  with a fixed array
  (issues [#31](https://github.com/eman/esphome-alpha-hwr/issues/31),
  [#34](https://github.com/eman/esphome-alpha-hwr/issues/34), PRs
  [#41](https://github.com/eman/esphome-alpha-hwr/pull/41),
  [#42](https://github.com/eman/esphome-alpha-hwr/pull/42)).

## [0.7.0] - 2026-07-05

### Added

- **Pump Link Status and Pump Link Fault text sensors** — two optional text
  sensors surfacing BLE link health. `Pump Link Status` reports a coarse
  connection-state enum (`Initializing`, `Connecting`, `Reconnecting`,
  `Connected`, `Unpaired`, `Unreachable`); `Pump Link Fault` shows the latched
  human-readable reason for the most recent failure (e.g. `Encryption Start
  Failed (0x61)`, `Remote Terminated (0x13)`), clearing to `None` once the link
  returns to `Connected`. The fault deliberately holds a significant
  auth/encryption failure through the reconnect churn so the cause survives to
  be seen without logs. Aimed at self-support, triage, and monitoring/automation
  (issue [#21](https://github.com/eman/esphome-alpha-hwr/issues/21), PR
  [#22](https://github.com/eman/esphome-alpha-hwr/pull/22)).

## [0.6.0] - 2026-07-03

### Added

- **`reconnect_settle_time` option** — device-agnostic fix for bond loss after
  a pump restart (issue [#5](https://github.com/eman/esphome-alpha-hwr/issues/5),
  PR [#11](https://github.com/eman/esphome-alpha-hwr/pull/11)). After a
  power-cycle the ESP32 could reconnect the instant the pump advertised, before
  its BLE stack was ready; the on-open encryption request then failed with
  `0x61` and ESP-IDF silently erased the stored bond. When set, the component
  waits for the pump to *reappear* after a disconnect and holds off
  reconnection for the configured settle time — timed from reappearance, so a
  5-second cycle and a 5-minute outage behave identically. The window applies
  only when a bond exists, so initial pairing is never delayed. Defaults to
  `2s`, which covers the measured 320-720ms pump vulnerability window with
  ~2.8x margin independent of host-side timing (issue
  [#14](https://github.com/eman/esphome-alpha-hwr/issues/14)); set `0s` to
  disable and restore the previous immediate-reconnect behavior.
- **Pump advertisement decoding at scan time** — new `PumpAdvertisementInfo`
  decoded from raw BLE advertisement bytes before any GATT connection, exposing
  `product_family`, `product_type`, and `product_version` (the BLE firmware
  discriminator between pump hardware revisions) plus the raw advertisement hex
  for debugging (PR [#10](https://github.com/eman/esphome-alpha-hwr/pull/10)).

### Fixed

- **Failed BLE opens no longer treated as real connections** — the base layer
  forwards `ESP_GATTC_OPEN_EVT` to components even when the open failed, so
  with the pump powered down or out of range the reconnect loop's stream of
  failed opens drove phantom `IDLE -> SERVICE_DISCOVERY` transitions, misfired
  encryption requests against dead connections, and could defeat the
  `reconnect_settle_time` window. The handler now runs only for successful
  opens (`ESP_GATT_OK` or `ESP_GATT_ALREADY_OPEN`, mirroring the base layer's
  success condition) and log-and-ignores the rest
  (PR [#9](https://github.com/eman/esphome-alpha-hwr/pull/9)).
- **`is_alpha_hwr_device()` matched the wrong advertisement field** — the check
  read Manufacturer Specific data at bytes 2/3, but the pump advertises its
  identity via Service Data (AD type `0x16`); detection now parses Service Data
  at the offsets used by the Python reference
  (PR [#10](https://github.com/eman/esphome-alpha-hwr/pull/10)).
- **CCCD write raced BLE encryption negotiation on bonded reconnect** — on
  fast-BLE chips, service discovery could complete mid-SMP-negotiation, so the
  notification-subscription (CCCD) write fired unencrypted and raced
  `ESP_GAP_BLE_AUTH_CMPL_EVT`; a rejection could end in bond erasure. The
  subscription is now deferred until `AUTH_CMPL` succeeds whenever an
  encryption request is still pending, tracked by two connection-scoped flags
  that reset on open, auth failure, and disconnect
  (issue [#12](https://github.com/eman/esphome-alpha-hwr/issues/12), PR
  [#13](https://github.com/eman/esphome-alpha-hwr/pull/13)).
- **Stabilize-to-auth timer survived a disconnect landing before auth started**
  — a disconnect inside the ~2s reconnect-settle window (before
  `authenticate()` had run) left the anonymous stabilize timer pending, so it
  later fired against whatever connection existed next. The timer is now named
  (`hwr_auth_start`) and explicitly cancelled on disconnect alongside
  `auth_.cancel()` (issue [#15](https://github.com/eman/esphome-alpha-hwr/issues/15),
  PR [#17](https://github.com/eman/esphome-alpha-hwr/pull/17)).
- **Initial-read-chain timers survived a disconnect mid-chain** — the post-auth
  chain of reads (device info, statistics, clock sync, schedule display,
  setpoints, event log/history/single-events) used anonymous `set_timeout`
  calls that kept firing against a new connection if a disconnect landed
  during the chain. A `read_chain_gen_` generation counter, bumped on
  disconnect, is now captured by every timer lambda at schedule time; each
  returns early if the generation no longer matches, mirroring the
  self-invalidation pattern already used by `auth_sequence_` and
  `scheduler_sequence_` (issue [#18](https://github.com/eman/esphome-alpha-hwr/issues/18),
  PR [#19](https://github.com/eman/esphome-alpha-hwr/pull/19)).

---

## [0.5.0] - 2026-06-30

### Fixed

- **BLE reconnect stability after pump power-cycle** — three bugs combined to
  produce a wedged reconnect loop that could only be recovered by reflashing
  ([#5](https://github.com/eman/esphome-alpha-hwr/issues/5), PR [#6](https://github.com/eman/esphome-alpha-hwr/pull/6)):
  - *Bond erasure on power-cycle*: encryption was requested at connection-open
    before the pump's BLE security stack was ready; a failed attempt caused
    ESP-IDF to silently erase the stored bond. Fixed by gating the encryption
    request on a bond-list check — only bonded devices request encryption
    immediately; unbonded devices defer to the pump's own `SEC_REQ`.
  - *Unreliable initial pairing*: a central-initiated pairing request on an
    unbonded pump returns `0x52 "Pairing Not Supported"`, causing the ESP32 to
    miss the pump's `SEC_REQ`. Fixed by the bond-check above.
  - *Stale state wedges reconnect*: `on_disconnected()` did not cancel the
    in-flight authentication handshake, leaving pending scheduler lambdas
    firing into the new BLE connection ("Stage 3: Sending extension packets"
    before the connection was open, "Service already running" on next
    auth-complete). Fixed by calling `auth_.cancel()`,
    `telemetry_service_.stop()`, resetting `initial_data_read_done_`, and
    flushing the transport command queue and pending response handlers on every
    disconnect.
- **Deprecated `ESPBTUUID::to_string()` replaced with `to_str()`** — two
  `ESP_LOGW` calls in `ble_connection_manager.cpp` used the deprecated
  `to_string()` method that ESPHome will remove in 2026.8.0
  ([#4](https://github.com/eman/esphome-alpha-hwr/issues/4), PR [#7](https://github.com/eman/esphome-alpha-hwr/pull/7)).
- **Spurious warnings for unsupported trend channels** — some pump models
  (e.g. ALPHA HWR 15-290 SU/T) do not populate the Temperature trend channel
  (Object 53 SubID 453), generating two `WARN`-level log lines on every
  startup. Wildcard command timeouts in the transport layer are now `DEBUG`;
  the trend-read timeout is reduced from 3 000 ms to 1 500 ms.

---

## [0.4.0] - 2026-05-18

### Added

- **DHW Demand Detector** (`dhw_demand` component) — standalone ESPHome
  component that detects genuine domestic-hot-water demand events by fusing
  pump telemetry with supplementary sensors (household flow meter, tank
  temperature, water-heater charge/in-use flag). Uses heuristic, threshold-based
  voting with no ML or external dependencies. Fully tunable via YAML
  `substitutions` without reflashing.
- **Derived pressure sensors** — inlet pressure and head-rate (kPa/s) computed
  from pump telemetry, exposed as Home Assistant sensors.
- **`dhw-demand-example.yaml`** and **`packages/dhw_demand_detector.yaml`**
  package for drop-in DHW demand detection.
- Protocol hardening and robustness improvements (Protofix commit).

### Fixed

- Motor enabled state separated from motor *running* state — pump-enabled
  binary sensor now reflects firmware state rather than RPM > 0.
- Head-rate computation refactored to callback-based approach; dt reset
  threshold raised from 3 s to 30 s to prevent false spikes on reconnect.
- Unit reporting corrections across multiple sensors.
- `esp32_ble_tracker` key restored in `alpha_hwr_base.yaml` (broken package
  after package restructure).
- `cppcheck` static analysis findings resolved across the component.

---

## [0.3.0] - 2026-02-22

### Added

- **Event log service** — reads the last 20 pump cycle events (start/stop
  timestamps, cycle numbers) on startup.
- **History trends service** — reads flow, head, temperature, and power-on-time
  trend data (last 10 and 100 pump cycles) from Object 53.
- **Operating statistics** — start count and total operating hours read from
  the pump on startup.
- **Cycle timestamps** — last 10 and 100 cycle Unix timestamps read from
  Object 88.
- **Quick Run / one-time schedules** — `quick_run_async()` method and
  Home Assistant button for immediate one-time pump activation.
- **Schedule management Lovelace card** — custom card for displaying and
  editing the weekly schedule directly in the Home Assistant dashboard.
- **ESPHome API services** — `alpha_hwr.set_schedule`, `alpha_hwr.quick_run`,
  and `alpha_hwr.sync_clock` callable from automations.
- **GENI error code descriptions** — human-readable labels for all pump alarm
  and warning codes in the Home Assistant UI.
- **Proportional Pressure control** — `set_proportional_pressure_async()`
  with m→Pa conversion matching the Python reference.
- **Cycle Time control** — `set_cycle_time_control_async()` for Object 91
  Sub 430 structured write.
- Boot-resilient initial data reads — device info, clock sync, event log, and
  history are re-triggered on reconnect if not yet completed.

### Fixed

- **Packet format for time sync** — rewrote `TimeService::set_clock_async()`
  to use proper Class 10 SET (`build_data_object_set`) with Type 322 header,
  replacing incorrect Class 7 style frame. Changed to fire-and-forget to
  eliminate 5-second transport queue blocking on every update cycle.
- **OpSpec 0x09 alarm/warning handling** — added handler for register-read
  response format; uses request-register echo to route alarms vs. warnings
  deterministically instead of poll-order toggle.
- **Duplicate frame builder** eliminated; CRC bug in control service packet
  builder fixed.
- Flash usage reduced by removing development logging (75.8% → within limits).
- Schedule display timeout and select-entity lag fixed.
- Pump switch now reads real state from passive notifications instead of
  assuming state after command.
- Setpoints (temperature range, flow, pressure) now read from passive
  notifications (Object 0x2F01) instead of failing Object 86 queries.
- Cycle time control wildcard response matching (pump's OpSpec 0x15 response
  does not carry Object/Sub IDs at standard bytes 6-7/8-9).

---

## [0.2.0] - 2026-02-15

### Added

- **Weekly schedule management** — read and write all 5 schedule layers;
  schedule display as a formatted text sensor in Home Assistant.
- **Device information service** — reads serial number, software version,
  hardware version, BLE version, and product name via Class 7 string commands;
  exposed as diagnostic text sensors.
- **Real-time clock service** — reads pump RTC (Object 94 Sub 101) and writes
  it (Object 94 Sub 100) once per day automatically after SNTP sync.
- **Control mode text sensor** — shows the pump's actual current control mode
  sourced from passive notifications (Object 0x2F01 Sub 1, OpSpec 0x0E);
  never shows a default/fake value before the pump reports its real state.
- **`packages/alpha_hwr_controls.yaml`** — optional package with all control
  UI entities (sliders, selects, buttons).
- **`hwr-pump-schedule-example.yaml`** — example configuration for schedule
  management.

### Fixed

- **Control service alignment with Python reference**:
  - `CONSTANT_FLOW` mode byte: `0x00` → `0x08`.
  - DHW On/Off suffix bytes corrected.
  - Temperature Range APDU size field: `0x09` → `0x0D`.
  - Constant Pressure now converts m → Pa (`× 9806.65`).
  - Constant Flow max range: 5.0 → 10.0 m³/h.
- **`set_mode()` simplified** — removed incorrect Class 3 fallback; pump
  always uses Class 10 for mode changes.
- **Object/Sub-ID byte order** in `Transport::try_dispatch_response()` — bytes
  6-7 are Object ID, bytes 8-9 are Sub-ID (was reversed).
- **Class 7 response matching** — matched by class byte only when
  `expect_obj_id == 0 && expect_sub_id == 0`, fixing device info reads.

---

## [0.1.0] - 2026-02-11

### Added

- Initial public component release.
- **BLE discovery** — identifies ALPHA HWR pumps by Grundfos Company ID
  (`0x0059`) with product family/type byte validation; falls back to service
  UUID (`0xFE5D`) detection.
- **GENI protocol authentication** — 3-stage handshake: legacy magic burst
  (3×), Class 10 unlock burst (5×), extension packets (EXT_1 + EXT_2).
- **Live telemetry streaming** — motor state (RPM, current, power, AC/DC
  voltage), flow rate, inlet pressure, media/PCB/box temperatures, alarms,
  and warnings; polled every 10 seconds.
- **Bidirectional pump control** — Start/Stop, mode selection (Auto, Constant
  Flow, Constant Pressure, Proportional Pressure, Temperature Range, Cycle
  Time, DHW On/Off), and setpoint adjustment.
- **Pairing support** — BLE bonding with `esp_ble_set_encryption()` and
  `ESP_GAP_BLE_SEC_REQ_EVT` acceptance; pairing status binary sensor.
- **Non-blocking BLE transaction manager** — command queue
  (`std::deque<Command>`) with 3-state FSM (`IDLE`, `SENDING_CHUNKS`,
  `AWAITING_RESPONSE`), 50 ms inter-packet pacing, and response matching by
  Object ID / Sub-ID with `SubID 0` wildcard quirk handling.
- **Layered, service-based architecture** mirroring the Python reference
  implementation: `core::` (transport, session, auth, BLE manager),
  `protocol::` (codec, frame builder/parser, telemetry decoder),
  `services::` (telemetry, control, schedule, sensor publisher).
- **Package-based YAML configuration** — `packages/alpha_hwr_base.yaml` and
  `packages/alpha_hwr_pairing.yaml` for modular device configs.
- **`hwr-pump-example.yaml`** and **`hwr-pairing-example.yaml`** reference
  configurations.

[Unreleased]: https://github.com/eman/esphome-alpha-hwr/compare/v0.8.0...HEAD
[0.8.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.7.0...v0.8.0
[0.7.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.6.0...v0.7.0
[0.6.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.5.0...v0.6.0
[0.5.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.4.0...v0.5.0
[0.4.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.3.0...v0.4.0
[0.3.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/eman/esphome-alpha-hwr/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/eman/esphome-alpha-hwr/releases/tag/v0.1.0
