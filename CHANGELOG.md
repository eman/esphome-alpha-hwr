# Changelog

## [Unreleased]

### Added

- **`write_bench.py chain`** runs several services over a single connection,
  resolving every service once up front. Each connection costs an
  `APIConnection` and its frame buffers on a node with ~72 KB free, and four
  stacked clients were enough to exhaust it (issue #127) -- a bench harness
  that reboots the node it is measuring. Note that the service-list encode in
  that crash's backtrace was the *victim*, not the cause: the failing
  allocation was at most ~48 bytes, so the heap was already gone.

### Changed

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
  firmware-build-only for no reason but a missing target. Each now has a test
  suite: the authentication handshake's packet sequence and its 1200 ms
  timer-only completion (the figure the deaf-node fix rests on, previously
  added up by eye); the publisher's presence gating, temperature bounds, head-
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

### Fixed

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

  Verified on hardware with a real draw. A tap opened with the pump off gave
  `deterministic_flow`; starting the pump armed `deterministic_continuation`;
  closing the tap released it 20 s later, the log naming the subtraction as
  what ended it. The pump was still turning when demand went `OFF`, so the
  meter was still reading loop flow above threshold -- the condition under
  which the tier previously held for the rest of the run.

  That run predates the zero-threshold and two-tick changes above, which came
  out of an adversarial review of the first version. It establishes that the
  tier releases on a stopped draw; it does not exercise the released
  thresholds. The small-draw and transient cases are covered by host tests.

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

- **A timed-out `pump_set_enabled` could stop a running pump on the next
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
  / `clear_vacation` services, and `pump_set_state`. All were already
  implemented and covered in `docs/`; only the README summary lagged.

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
