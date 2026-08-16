# Full-codebase audit — 2026-08-12

Target: `main` @ `fd77d53`, clean tree. ~33k tracked LOC.
Method: mechanical baseline → 11 parallel single-lens reviewers → 13 adversarial skeptics.
93 candidates raised; 13 sent for refutation; this report keeps what survived.

## How to read this

Every claim below went through a skeptic whose brief was to **refute** it, default to refuted, and
reproduce independently rather than trust the finder's probe. Verdicts:

| Tier | Meaning |
|---|---|
| **CONFIRMED** | Survived a genuine refutation attempt, usually with an independent probe. |
| **REFUTED** | The mechanism may exist, but the claimed consequence or reachability does not. |
| **UNVERIFIED** | No skeptic ran. Treat as a lead. |

The verification pass changed **twelve of fourteen severities** and rewrote three fixes. That is
the most useful thing in this document — the finder pass alone would have sent you after the wrong
problems. Notable corrections are called out inline.

## Summary

The write layer's core invariant holds: a fuzz probe over 40 seeds × 60 random operations crossed
with disconnects, dropped responses and re-entrant submits found **zero** violations of "exactly
one terminal event" (AGENTS §8.4 rule 4). Rules 1 and 2 check out too. NaN handling inside
`dhw_demand_logic.h`, `codec.cpp` endianness, and the codegen↔C++ surface are all clean.

What actually needs attention, in order:

1. **The Lovelace card cannot write to the device at all.** Confirmed and *strengthened* under
   scrutiny.
2. **Two closure leaks that OOM the node.** The only finding that survived at P1 on severity
   grounds — and the fix originally proposed for it does not work.
3. **The test suite validates a copy of the code under test.** Proven by mutation.
4. **A cluster of P2 correctness bugs** where a write reports success it did not earn.

Post-verification counts: **0 P0, 4 P1, 15 P2, 12 P3, 1 P4**, plus the P2/P3 tail that was not
sent for refutation. Every P0/P1 candidate raised by the finder pass was verified; none survived
at P0.

---

## P1 — survived refutation

### 1. The card omits `op_id`; every write it makes is rejected before reaching the device
`homeassistant/www/alpha-hwr-schedule-card.js` — 7 call sites — **CONFIRMED, strengthened**

All 16 services declare `op_id`; the card contains zero occurrences. Settled without relying on
Home Assistant internals: `.venv/…/aioesphomeapi/client.py:1806` does an unguarded
`data[arg_desc.name]` → `KeyError`, and `UserServiceArg` has no optional concept in the wire
protocol. HA core's `manager.py` independently builds `vol.Required` for every arg with no
`vol.Optional` branch. **Nothing reaches the device and no settle event ever fires.**

The skeptic made this worse than reported in two ways. `op_id` landed 2026-07-18 and the card was
edited **four times afterwards** (07-23, 07-24, 07-26, 07-27) without ever sending it — this is not
update lag. And the card is live: `README.md:324-382` documents installing it, and it is
instantiated in the maintainer's own dashboard config.

Symptom: **silent no-op with an optimistic UI.** No toast — a rejected `callService` from a custom
card surfaces only as an unhandled console rejection. `_saveChanges()` clears `_pendingChanges` and
re-renders immediately, so the grid paints the edit as applied, then silently reverts on the next
sensor update.

*Scope correction:* 7 broken sites, not 8 — `_toggleSchedule` uses core `switch.turn_on/off` and
still works. The card exercises only 6 of the 16 services.

### 2. Two closure leaks large enough to exhaust the heap
`history_service.cpp:66` (P1) · `schedule_service.cpp:1123` (P1) · `event_log_service.cpp:92` (P2)
— **CONFIRMED ×3**

`auto read_next = make_shared<function<…>>(); *read_next = [this, …, read_next](…)` — the closure
captures by value the `shared_ptr` that owns it. 100/100 sentinels survive over N=100 repetitions
at each site: linear, not one-off. Measured 1053 B / 437 B / 389 B per call on host.

`trigger_initial_data_reads()` *is* idempotent per connection, but its flag clears on every
disconnect (`alpha_hwr.cpp:93`), so all three repeat per authenticated reconnect. Scaled to the
ESP32-C3: **150–260 KB/hour under sustained link flapping** against ~150–250 KB usable heap — OOM
in 1–3 hours. Even a healthy link at one reconnect/day exhausts heap in 4–6 months. Given the #127
production OOM history, this is the finding to fix first.

`schedule_service.cpp:1123` is the only site that repeats **without** a reconnect:
`refresh_single_events` is a public HA service the card fires 3 s after every save.

> **Fix correction — the obvious fix does not work.** Nulling the `shared_ptr` in the terminal
> branch is insufficient: the cycle forms at closure-assignment time, before any I/O, and
> `Transport::reset()` (`transport.cpp:268`, called on disconnect) clears the command queue
> **without invoking callbacks**, so an abandoned chain never reaches its terminal branch and leaks
> anyway — pinning the partial result vector too. That is precisely the flapping case that drives
> the number.
> **Correct fix:** capture a `weak_ptr` in the outer closure, `lock()` at entry, and let the inner
> transport callback hold the strong ref, so it dies with either the final `pop_front` or the
> `clear()` in `reset()`. `schedule_service.cpp:552` already uses the right shape.

*Severity split:* `event_log_service` downgrades to P2 — its closure allocates inside the metadata
callback, gated on session-ready + metadata success + non-empty log; both negative branches
allocate nothing.

### 3. The test suite validates a duplicate of the code under test
`tests/protocol.h:45` — **CONFIRMED by execution**

A reviewer mutated `codec.cpp`'s CRC init and swapped CRC bytes in
`frame_builder.cpp::build_class10_read` in an isolated worktree — a total protocol break where
every emitted frame carries a wrong CRC — and `make test` produced **byte-identical output to
baseline, 21/21, exit 0**.

`test_protocol.cpp` validates `tests/protocol.h`, a private copy of the CRC table and packet
builder, and `tests/README.md:58` instructs contributors to keep mirroring production into it. Two
more instances: `test_transport_matching.cpp` reimplements the matching predicate locally, and
`test_control_state.cpp` runs 116 assertions against a hand-written `ControlService` replica that
**has already drifted** — production caches setpoints and maintains fix-#53 remote-mode state; the
replica does neither.

This is the most consequential finding here, because it silently weakens every guarantee the suite
appears to give.

### 4. Nothing tests `parse_frame`, and the suite depends on a missing CRC check
`frame_parser.cpp:128`, `tests/test_write_operations.cpp:303` — **CONFIRMED**

No test calls `parse_frame`. Its Class 2/3 branch guards `len > 6` then computes
`payload_len = len - 8`, wrapping to `SIZE_MAX` at `len == 7`, reachable because the `len >= 8`
guard at `:32` runs *before* the clamp at `:65`. Latent only because no caller currently reads
`ParsedFrame::payload`.

Separately: every response fixture in the suite ends in a literal `0xAA 0xBB` — a garbage CRC — and
is accepted, because `Transport::try_dispatch_response` never validates CRC (only
`telemetry_service.cpp:94-97` does). **Fixing the CRC gap would fail 300+ assertions.** Understand
that coupling before attempting it. The underlying gap is real: every control, schedule,
single-event, event-log and device-info payload is parsed from unverified bytes, including the
readbacks that decide write verdicts.

> **Resolved in PR #165 (2026-08-13).** The count was **144**, not 300+
> (`test_write_operations` 136, `test_schedule_service` 5, `test_transport_fsm` 3), and they funnel
> through about five injection points rather than living in 144 separate fixtures — so
> `tests/fixture_crc.h` stamps a real CRC at each, using the *production* `calc_crc16_read` so the
> suite can only pass when fixtures and firmware agree. Replaying 36,394 captured notifications
> through the real `Transport` completes 17,624 frames and drops 3 (0.017%), all genuine
> corruption: no alternate CRC window matches them.

---

## P2 — confirmed, but narrower than first reported

### 5. A timed-out `set_pump_enabled` can stop a running pump on the next setpoint write
`write_operation_service.cpp:531` → `control_service.cpp:612` — **CONFIRMED, P1 → P2**

`note_mode_commanded` sets a pending flag; `note_enabled_commanded` sets `pump_enabled_valid_ =
true` and **no such flag exists in the class**. Nothing rolls it back on TIMEOUT, and
`with_resolved_enabled_state` short-circuits on exactly that flag — defeating the guard its own
"Fix #45" comment describes.

Reproduced with byte-level evidence. Byte 12 is the only difference:
```
healthy:  0A 90 56 00 06 01 2F 01 00 00 07 00 00 02 45 3B 80 00   flag 0x00 START
poisoned: 0A 90 56 00 06 01 2F 01 00 00 07 00 01 02 45 3B 80 00   flag 0x01 STOP
```
The mirror case force-**starts** a stopped pump.

Downgraded because the exposure window is **~10–41 s**, not indefinite: any successful
`get_mode_async` rewrites the flag from the pump's `operation_mode` byte, and the #54 control-state
poll runs it every 30 s. Harm needs a four-way conjunction (pump ACKs but ignores + readback fails
+ link survives + setpoint write inside the window), and the poisoned value is always the user's
own last explicit command. **But** once the frame lands the pump really is stopped and the cache
now matches reality, so with the schedule off it stays stopped until a human acts.

> **Fix correction:** the terminal event comes from the **watchdog** (`arm_watchdog_` → `finish_`),
> not `confirm_enabled_`'s `!success` branch, so a rollback there misses the dominant path. Put it
> in `finish_()`'s `SET_PUMP_ENABLED` case: clear `pump_enabled_valid_` on TIMEOUT/SUPERSEDED.
> Leave REJECTED alone — it only reaches its verdict via a successful readback that already
> corrected the cache.

### 6. `upload_schedule` settles ACCEPTED on an unconfirmed enable write
`write_operation_service.cpp:1519-1540`, `:352` — **CONFIRMED (i), REFUTED (ii), P1 → P2**

`upload_apply_enabled_` spells its parameter `bool /*sent*/` and goes straight to `CONFIRMING`.
`confirm_upload_` walks layers only; the enabled flag lives in Sub 1, which no layer readback can
carry. `build_result_` reports the *request*. Not theoretical: `set_state_async` calls
`on_sent(true)` even when `acked == false`, deferring to a `poll_state_async()` readback that
upload never performs. A full 3 s timeout of the enable write still settles `accepted,
enabled=true` with the pump off. The control experiment: the dedicated `set_schedule_enabled` op
against the same simulated pump correctly settles `rejected`. Upload is the outlier.

Direction (ii) — the alleged "identical-grid no-op" branch — is **refuted as fabricated**. The
early exit keys on the enabled state, never the grid.

**The skeptic found something worse.** `set_state_async` (`schedule_service.cpp:353-355`) writes
`schedule_enabled_` from the *request* even on a closed window, discarding the pump's truthful
echo. So a retry — the documented idempotent recovery — hits the early exit, issues zero overview
writes, and still settles `accepted`. And `current_hash()` folds that poisoned flag into the event:
identical `v1:673dd2d1104de5b5` whether the pump's schedule is on or off. The one artifact meant to
catch the lie agrees with it.

*Fix:* give the enable leg the readback `confirm_schedule_enabled_` already has.

### 7. A late response is delivered to the next command
`transport.cpp` — the `AWAITING_RESPONSE` timeout in `Transport::loop()`, and the `WILDCARD MATCH`
and `Exact match` branches in `Transport::try_dispatch_response()` —
**CONFIRMED, P1 → P2 — MECHANISM CORRECTED, see the note below**

> *Citation note.* This section originally cited `transport.cpp:501` and `:507` for the wildcard and
> exact-match paths. Neither resolved even at the audit's own commit (`e28688c`), where they were
> `return false;` and a byte extraction; the paths were then at `:520` and `:526`. A first attempt to
> fix this substituted `:581`/`:589`, which were also wrong by the time they were written — the
> section is edited more often than the line numbers are re-derived. Hence the search-by-name
> citation above, which does not rot. Where the body text below quotes `:501`/`:507`, it is quoting
> the original claim, not asserting a location.

> **Correction (2026-08-13, from the capture investigation behind PR #166).** The collision is real
> but this section gets *why* wrong, and the wrong reason points at the wrong fix.
>
> A GENI response carries **no Object ID and no Sub-ID**. Bytes 6-9 are `[00][TypeH][TypeL][Version]`
> — the object *type* and version. Byte 6 is `0x00` in 100% of the 21,236 captured Class 10
> responses long enough to carry a type header (the remaining 484 are 9-byte short ACKs with none).
> A request names an object and a sub-id (`0A 03 [obj] [subH] [subL]`) and carries no type at all, so
> the reply is not echoing the request: it is naming the type of whatever object was asked for.
>
> So matching discriminates object **types**, never *instances* of a type — and that is true on the
> exact-match path too. This section's claim that the collision "holds only for the exact-match path"
> is therefore backwards: the exact-match sites collide identically wherever siblings share a type.
> Verified byte-for-byte: schedule layers Sub 1000-1004 all answer `00 00 DE 01`, single-event slots
> Sub 900-904 all answer `00 00 DC 01`, event-log entries Sub 10200-10219 all answer `00 00 F4 02`.
> The latter two are not mentioned here at all.
>
> Consequently "stop using the wildcard" does **not** fix the collisions reproduced below: those are
> same-type siblings that no field matching can separate. It is still worth doing for a different
> reason than this section gives — the wildcard accepts *any* Class 10 frame, including the passive
> `00 01 2F 01` mode notifications the pump emits constantly (4,306 in the captures), so narrowing it
> removes a genuine cross-*type* misattribution risk. Note the wildcard is not confined to singleton
> reads: Object 53 trends (`history_service.cpp:100`, `:189`) and the clock read
> (`time_service.cpp:47`) both use it, and trends are one of the three collisions below.
>
> Observed exposure is also **zero**: across 24,224 paired request/response exchanges no response
> arrived later than 268 ms against a 3000 ms timeout (excluding one 154 s gap that is a capture
> artifact, not a reply). The misattributions visible in the captures come from the
> phone app *pipelining* requests, which this firmware never does (single `command_queue_.front()`,
> `AWAITING_RESPONSE` gate, 50 ms pacing).
>
> What the investigation *did* find is a real bug this section's framing hid: the Object 86 Sub 7
> mode read passed its two matching arguments in the wrong order and matched only through a fallback
> branch. Fixed in PR #166.

`Transport` keeps no per-command identity; matching is against `command_queue_.front()` only, and
GENI frames carry no sequence number. The timeout path records nothing about the command that died.

> **My own analysis was wrong here.** I reasoned that differing Object IDs would disqualify a
> collision. False for the dominant path: the full Class 10 wildcard at `:501`
> (`expect_obj_id==0 && expect_sub_id==0`) accepts any non-register-read Class 10 packet and never
> consults the Object ID. It holds only for the exact-match path at `:507`.

Reproduced collisions, all issued back-to-back by production code: Object 53 trend reads
(**Flow's payload published as Head**); five Class 7 device-info strings (**product name stored as
serial number**); schedule layer reads Sub 1000..1004 (**layer 0's image cached as layer 1,
reported success**).

**`find_(seq)` does not immunise the write path** — it guards which `Operation` a callback mutates,
while the verdict is computed from the payload, which is exactly what gets swapped. A confirm
readback can settle on the periodic poll's pre-write snapshot.

P2 because it needs a genuinely *late* response, not merely a timeout; the documented read failure
mode is silence, which produces no stale frame.

*Refuted sub-claims:* the Class 3 run/remote-mode pair is unreachable, and Obj 91 Sub 430 vs 421 is
immune via the OpSpec 0x15/0x0D workaround.

### 7b. `is_register_read` is a length blocklist wearing a format filter's name
`transport.cpp` — the two `is_register_read` tests, in `Transport::try_dispatch_response()` and in
the registered-response-handler path below it — **NEW, added 2026-08-13, P2**

Not in the original audit; found while investigating finding 7.

Byte 5 of a response is the APDU **body length**, not an operation code: `byte5 == total_len - 8`
held for all 24,233 CRC-valid captured inbound frames without exception. (More precisely it is a
flag bit plus a length — the short-ACK branch in `try_dispatch_response()` reads `0x81` as a status,
i.e. bit 7 set over length 1. No captured Class 10 response has bit 7 set, so the equality holds
across the corpus but is a statement about the corpus, not about the format.) So

```cpp
bool is_register_read = (opspec == 0x30 || opspec == 0x2B || opspec == 0x14 ||
                         opspec == 0x2E || opspec == 0x2D || opspec == 0x09);
```

does not mean "this is a telemetry register read". It means **"this response's body is 48, 43, 20,
46, 45 or 9 bytes"** — and any such response is silently dropped for command matching, so the
command it was answering times out instead.

This is not hypothetical: event-log entry replies are 20 bytes (`0x14`) and already trip it.
`event_log_service.cpp:139` works around it with `allow_register_read=true` rather than the filter
being fixed. Every other read is one payload byte away from the same fate — cycle timestamps
currently answer `0x2F` (47), with `0x2E` (46) and `0x2D` (45) both on the list.

**RESOLVED 2026-08-13.** The investigation this called for changed the severity, the blast radius and
the fix, so all three are recorded here.

*The blocklist is roughly, but only roughly, telemetry-shaped.* Four of its six values (`0x30`,
`0x2B`, `0x14`, `0x09`) are reply sizes of registers `TelemetryService::poll()` reads and match cases
in the OpSpec switch in
`telemetry_service.cpp::on_packet` (`0x30` motor state, `0x2B` flow/pressure, `0x14` temperature,
`0x09` alarms/warnings). The other two, `0x2E` and `0x2D`, match nothing in that switch; and `0x13`,
which the switch *does* handle, is absent from the list. So it neither covers telemetry nor is
limited to it. It was never a format test; it was approximately "the lengths our telemetry replies
happen to have", which holds only for as long as that register set and the pump's payload sizes
both do.

*It cannot be deleted, and the captures cannot supply a discriminator.* Telemetry reads are queued
as Class 10 **wildcard** commands (`expect 0/0`), so without the guard a telemetry reply satisfies
whatever wildcard command is at the head of the queue. The obvious discriminator does not transfer:
in the reference captures the response class always equals the request class (24,223 of 24,224 paired
exchanges) and the phone app read telemetry as **Class 2**, so `data[4]` sorts the app's traffic
perfectly — but this firmware reads telemetry as Class 10 (`build_class10_read`), so the same byte
sorts none of ours. There is no header-level discriminator available to us.

*The defect was worse than described above.* The filter ran ahead of **all** matching, so it applied
to exact-match reads too: a reply carrying exactly the type the command asked for was discarded
purely because its body length collided, and the command then timed out with its answer already in
hand. A host probe against the shipped transport confirms it — with the command expecting type
`DA01`, replies of body length 20/43/45/46/48/9 were dropped while 14/25/35/47/49 matched, the type
field never consulted. So this was not a wildcard-path problem; it was every read.

*Fix:* the guard now applies only to wildcard commands. When a command names a type, that type is
the discriminator and length gets no vote; wildcard commands, which have nothing else to match on,
keep the guard unchanged. `event_log_service.cpp`'s `allow_register_read=true` workaround is dropped
as a result — the exact-match read now gets its own 20-byte answer. Pinned by
`test_length_collision_does_not_veto_a_type_match` and two entries in `tools/mutation_check.sh`
(`register-read-vetoes-type-match`, `register-read-guard-removed`), each verified to fail the suite
in the correct direction.

### 8. The deaf node reports "Connected" forever
`ble_connection_manager.cpp` — the CCCD return-code check in
`BLEConnectionManager::subscribe_to_notifications()` and the `ESP_GATTC_WRITE_DESCR_EVT` case in
`handle_gattc_event()`; `auth.cpp` — **CONFIRMED, P2 — RESOLVED 2026-08-14**

`auth.cpp` is a pure scheduler chain; nothing inspects a reply, and READY arrives 1200 ms after
`authenticate()` regardless of received data. Both CCCD-failure paths reach it — the synchronous
return code only logs then falls through to `subscribed_callback_()`.

I expected the link-status ladder to catch this. **It does not:** `is_ready()` is the ladder's
*first* rung, so this state reports "Connected" forever and keeps refreshing `link_last_open_ms_`,
so it can never fall through. The user sees Connected + Pairing on, Pump Ready off, every sensor
frozen, and a control-cache retry every 5 s indefinitely.

> **RESOLVED** — `components/alpha_hwr/link_watchdog.h`, wired in
> `AlphaHwrComponent::check_link_liveness_()`.
>
> The fix is a liveness check, not the handshake gate the finding implies. Gating READY on the
> async `ESP_GATTC_WRITE_DESCR_EVT` status trades a falsely-ready node for one that may never
> become ready; gating it on "a notification arrived during auth" appears to hold on this pump —
> notifications are received during the handshake, which is why no default control mode is
> published at setup — but that rests on a single specimen, and nothing committed to this repo pins
> the timing, so it is an observation, not a measured margin. Both fail closed on a variant that
> stays quiet until first polled. Instead a single timer, seeded at connection-open and refreshed
> on every received notification, tears the link down when nothing arrives within `data_timeout`
> (new option, default 60 s, 0 disables).
>
> **The remedy had to be a disconnect, not a session-state transition.** Recovery in this component
> is driven by the BLE disconnection callback, not by session state, so transitioning to ERROR
> would swap a falsely-ready node for a permanently stuck one — strictly worse. `force_disconnect()`
> follows the precedent already in `ble_connection_manager.cpp` for a bonded reconnect whose
> encryption failed, and latches its reason through the ensuing reconnect loop so the Pump Link
> Fault sensor is not overwritten by the "Local Host Terminated" event it provokes.
>
> Timing the window from the open rather than from READY also covers the finding's unstated half:
> the other four subscription paths return *without* calling `subscribed_callback_()` at all,
> parking the session in SUBSCRIBING forever. Same observable, same remedy.
>
> Sizing is not a guess — a READY link is polled every 10 s and `update_interval` is rejected as an
> invalid option (verified against the schema), so no configuration can widen the healthy gap past
> the 60 s budget. Worst case to first data is the handshake, not steady state: 17.2 s by the
> constants, 5.90/6.17/5.94 s to READY measured across three reconnects on hardware. Benched both
> directions against the final tree — at a deliberately short 5 s budget the full path fires,
> force-disconnects (reason 0x16), latches the fault, reconnects, re-authenticates and returns to
> READY with the fault released; at the shipped 60 s it did not fire once in a 5m23s soak carrying
> 404 sensor publishes with no drops. Pinned by `tests/test_link_watchdog.cpp` (18 assertions) and
> two entries in `tools/mutation_check.sh`.
>
> **What it does not do**, per the skeptic pass: it recycles a deaf link rather than making one
> legible. READY is still reached without data, so a *permanently* deaf pump cycles ~60 s
> looking connected against ~6 s reconnecting, and the link status reads "Connected" for most of
> that. The finding's headline symptom is therefore corrected only in the sense that the node now
> keeps trying; a user watching the sensors sees flapping rather than a steady fault. Gating READY
> on received data would fix that too and remains the deeper fix — it was declined here for the
> single-specimen reason above, not because it was overlooked. The rollover mutation is the one worth noting: written the obvious way
> as `now > last + timeout`, the predicate agrees with the shipped form on both of the rollover
> assertions I first wrote, and a node near 49 days of uptime would be torn down on every tick. The
> test that discriminates them was added only after checking the mutation actually failed.

### 9. `dhw_demand` trusts a stale pump-OFF reading indefinitely
`dhw_demand.cpp:333-346` — **CONFIRMED (consequence), REFUTED (mechanism), P2**

The claimed mechanism — "the BLE-gap forward-fill is dead code" — is **refuted three ways**. Most
decisively, `packages/dhw_demand_detector.yaml`, the documented standalone entry point, wires no
`motor_speed` and no `motor_current`; `read_sensor_` returns NAN for a nullptr, so
`pump_state_known` is permanently **false** and the else-branch runs every tick forever. The exact
opposite of the claim.

The false positive underneath is real but reached through **staleness, not NaN**: the last publish
before a BLE drop was 0 RPM, `dhw_demand` trusts it indefinitely, and loop flow is read as
household demand — `deterministic_flow` at 100%.

> **Fix correction:** belongs in `dhw_demand`, not `alpha_hwr`. A probe showed that publishing NaN
> on disconnect — the finder's implied fix — *still* yields `deterministic_flow` at 100%, because
> `pump_confirmed_off` re-asserts confirmed-off from the forward-fill. It would cost every pump
> entity going "unknown" in HA on each routine reconnect and buy nothing. The staleness machinery
> already exists (`reading_is_fresh()`) but is applied only inside `decide_pump_on()`, never to the
> motor channel that selects the branch. Stale-ON is already guarded; only stale-OFF is exposed.

*Also:* the finder misread AGENTS §11.8 rule 2, which governs *unconfigured* inputs — the
nullptr→NAN path satisfies it exactly. It was never falsified.

> **RESOLVED** — behaviour in `6632e2e` and `d3560d9`; the test gap closed 2026-08-14.
>
> The fix landed as the correction above prescribed: each motor channel is masked by its own age
> before anything reads its value, and a third regime — *frozen*, meaning reporting but not
> refreshing — sits between "fresh" and "genuine NaN gap". Frozen assumes the pump is ON and refuses
> to assert `pump_confirmed_off`, which is what had reopened the pump-OFF branch after the staleness
> test had already rejected the reading.
>
> **The gap was that none of it was tested.** The decision lived in `dhw_demand.cpp`, which no host
> test compiles — only the pure header is linked — so the branch selecting between the pump-ON and
> pump-OFF demand paths had zero coverage, in a component whose logic is otherwise well covered.
> Extracted verbatim into `decide_pump_state()` in `dhw_demand_logic.h` and pinned by 35 assertions,
> the load-bearing one being that a frozen 0 RPM does not assert confirmed-off. Five mutations
> (`frozen-motor-asserts-pump-off`, `motor-staleness-mask-removed`,
> `motor-current-staleness-mask-removed`, `motor-speed-on-threshold`,
> `motor-channel-precedence-swapped`) confirm the tests bind the shipped predicate; the last three
> were added after a skeptic pass showed they survived the first round of tests.
>
> **Reproduced on hardware.** The frozen regime is not a hypothetical: forcing a BLE drop
> (`data_timeout: 5s`) and holding the reconnect past the 30 s motor-staleness bound
> (`reconnect_settle_time: 90s`) put the detector into it 17 times over one run. Throughout, it
> reported `pump_on_uncertain` at 50 % confidence — the safe answer the design predicts — returning
> to `deterministic_idle` at 100 % once readings were fresh again. **`deterministic_flow`, the false
> positive this finding is about, occurred zero times.**
>
> The extraction also collapsed a duplicate: `pump_confirmed_off` was computed a *second* time,
> thirty lines below `pump_on`, from the same inputs. I first wrote this up as a latent defect —
> "two copies that could disagree" — and the skeptic pass demolished that. They were *structurally
> incapable* of disagreeing: divergence needs a member write between the two blocks, and their
> branch predicates are mutually exclusive, so the second block can never observe the first block's
> update. A differential harness over 25,600 input combinations found not one mismatch, and further
> showed `pump_confirmed_off == !pump_on` on every one of them — all three arms collapse to that.
> Worth removing as duplication; it was never a bug, and calling it one was me dressing up a tidy-up.
>
> That same harness is the evidence the extraction is behaviour-preserving, comparing 13 outputs per
> tick (including both log-branch selections and the downstream edge stamps) against the pre-change
> logic transcribed from `main`.

### 10. The pump-on continuation tier cannot release during a run
`dhw_demand_logic.h:418` — **CONFIRMED, P1 → P2, and the AGENTS directive is NOT violated**

`pre_pump_on_flow_` is written in only three places, none inside a pump run, and the tier's only
"still drawing?" test is raw meter flow > 0.3 GPM. That exit is physically unreachable: every
pump-on meter reading the repo records — 0.71 GPM at 1650 rpm, 1.31 no-draw median, 1.45
pre-shutdown, 2.22 no-draw p90 — leaves the continuation active, the lowest being 2.4× threshold.

Reproduced: a 1.80 GPM draw that stops 5 minutes into a 30-minute run gives **180/180 ticks
`deterministic_continuation`, 150 of them after the draw ended**, `demand_level` 0.72 while the
subtraction reads exactly 0.00 GPM. `session_duration` 1870 s against a 360 s true draw.

> **Three framing corrections.** Not permanent — `dhw_demand.cpp:370` clears it on the first
> confirmed pump-off tick, so it is bounded by the run. Not a regression — `git show 6f89f2d~1` has
> identical logic inline; #150 only moved the predicate into the header. **And AGENTS §11.4 is not
> breached:** the "no pump-on rule may key off raw meter flow, ever" sentence is immediately
> followed by its own exemption for this tier, and §11.4 item 1 documents the raw-flow term
> verbatim. The code implements the spec as written.

What this really is: **the spec has a hole**, and the directive's own evidence proves it — the
no-draw distribution cited to show no threshold exists is exactly why "still above threshold now"
has no discriminating power. A spec discussion, not a revert.

*Test gap is genuine:* `test_dhw_demand_logic.cpp:652` asserts the exit exists, captioned "The draw
stopping ends the continuation" — and passes only because it uses a 0.1 GPM reading below every
value the repo has recorded with the pump running.

> **Fixed.** The spec hole was closed rather than the tier reverted. `pump_on_continuation_verdict()`
> now releases on the subtraction — tier 2's own oracle under tier 2's own guards, used to end a
> claim rather than start one, so the raw-flow prohibition is untouched — reading at or below
> `pump_on_demand_flow_threshold`, and that release *retires* the capture so a later loss of the
> subtraction cannot resurrect a disproved claim. Because the subtraction is unavailable below
> `pump_on_demand_min_speed_rpm` (a pump clamped to 1650 rpm never offers one), a new
> `pump_on_continuation_max_seconds` (300, `0` disables the tier) bounds the case where nothing can
> contradict it. `AGENTS.md` §11.4 carries the three exits and why the old one was unreachable. The
> 0.1 GPM test was replaced with the four measured pump-on meter readings, plus the audit's own
> 30-minute repro end-to-end through the component: 180/180 continuation ticks before, 30 after.

### 11. The single-event display regex cannot match the firmware's output
`alpha-hwr-schedule-card.js:226` — **CONFIRMED, P2-high**

The split-token escape hatch does not save it: the code splits on `\n` then matches per line, and
the ` (run)`/` (off)` suffix sits at each line's end. Executed with `node`: suffixed forms no match.
`752b5c8` (2026-07-22) added the suffix; the regex dates to `3038b81` (2026-02-15).

Symptom: **silently empty**, indistinguishable from "no events exist." And an effect the finder
missed — `_clearSingleEvent` is reachable only from a rendered row, so single-event deletion is
dead in the UI *before* the `op_id` bug would break it.

> **Resolved in `7f5037a` (2026-08-13)**, in the same commit as finding 1 — the `op_id` fix had to
> parse the event list to reach the delete path at all. The regex now takes an optional
> ` (run)`/` (off)` suffix and carries the action through. Re-verified by execution against the
> exact `format_single_events_display` output, and now pinned by a host test
> (`tests/js/test_schedule_card.js`) so it cannot regress unnoticed a third time.

### 12. `_saveChanges` is structurally unsafe
`alpha-hwr-schedule-card.js:1653` — **P2, unverified in detail**

Clears `_pendingChanges` before confirmation; issues up to 35 independent writes where
`upload_schedule` exists to replace them; refreshes on a blind 3 s timer against a 150 s watchdog.

> **Resolved in PR #182 (2026-08-15).** Verification split the three claims apart, and only two of
> them survived.
>
> **Claim 1 (the optimistic clear) is the bug, and worse than stated.** Nothing waited for
> anything: the edit was dropped at call time, so a `rejected`, `invalid` or `timeout` write was
> indistinguishable from an accepted one — both ended with the user's change gone from the grid
> and no message anywhere. The card had the correlation handle all along (`_callService` generates
> a unique `op_id` and the device answers every write with exactly one
> `esphome.alpha_hwr_write_settled` carrying it), and simply did not subscribe. It does now: edits
> stay pending until their own op settles, failures are named on the card, and an edit re-made
> while its write is in flight survives the older write's confirmation (identity check, not
> equality — every edit path stores a fresh array).
>
> **Claim 3 (the blind timer) is real, and the number quoted is the wrong one.** 150 s is
> `WATCHDOG_UPLOAD_MS`, which this path never uses; `set_schedule_entry` carries
> `WATCHDOG_SCHED_ENTRY_MS` = 20 s, and writes are queued, so a batch's ceiling is 20 s × batch
> size. The 3 s read still lands inside it and returns pre-write state. The refresh now fires when
> the batch drains, with a backstop derived from the firmware's own budget rather than a guess.
> The single-event paths had the same defect against a *60 s* watchdog, plus an optimistic local
> delete; both fixed the same way.
>
> **Claim 2 (use `upload_schedule`) is rejected.** "Up to 35 writes" is reachable only by editing
> every cell — the loop iterates `_pendingChanges`, not the grid, so ordinary use is one to three
> writes, *fewer* than an upload. And `build_layer_image` memsets each layer before filling it, so
> an upload clears every (layer, day) cell not listed. `_rebuildSchedule` silently skips a layer
> sensor that is malformed or not yet cached, so uploading the card's merged view would erase
> whatever that layer holds on the device. The batched path would trade a display bug for data
> loss; the per-entry path writes only what changed.

> **Two defects in the fix itself, both found by the skeptic pass, both mine.**
>
> *The backstop could fire while the write was still in progress.* The per-command watchdog
> budgets bound an operation's time at the *head* of the device queue — `arm_watchdog_` runs from
> `start_front_`, so a queued operation carries no timer until it gets there — and the queue is
> shared with every other write source, including the card's own untracked `refresh_schedule`
> (30 s). A one-cell save behind a refresh would hit its 25 s backstop first, report "no
> confirmation" for a write the device went on to accept, and then discard the real settle. Fixed
> with the worst foreign budget as slack plus re-arming on progress; erring long is correct here,
> since a late backstop only delays a message while an early one manufactures a false failure.
>
> *A re-attach mid-write stranded the card permanently.* Lovelace re-appends an existing card
> element whenever a masonry view re-columns — a resize, the sidebar, entering edit mode — firing
> `disconnectedCallback` then `connectedCallback` on the same instance. Teardown cancelled the
> backstop, and `_armBackstop` was reachable only from `_saveChanges` and `_trackSingleEvent`,
> both of which refuse to run while writes are outstanding. The card rendered "saving…" forever
> with Save *and* Discard disabled, recoverable only by reloading. The deadline is now absolute
> and resumed on re-attach.
>
> A third, smaller one: a Quick Run wiped an unread schedule failure, because `_writeErrors` was
> cleared wholesale rather than per surface. And the guard that refuses a second single-event
> write was invisible — the buttons still looked live and swallowed the click. Both fixed.

**Tests.** 46 tests / 145 assertions in `tests/js/test_schedule_card.js` — the card's first
automated coverage of any kind.

> **The first version of this suite was unsound, and only an adversarial pass found it.** My own
> 8 mutations all died, which proved nothing: a third skeptic ran 116 and buried ~50. The harness
> left `_showQuickRun` false and `hass.states` empty, so `_renderQuickRunPanel` returned `''` in
> every test — the entire single-event UI could be deleted with the suite green, and so could the
> Save button's `data-action`. The XSS test was worse than useless: its payload had exactly one
> `<`, one `>` and one `"`, so it passed against a non-global `replace` that is a live injection,
> demonstrated by rendering a two-tag payload through the mutant. Both are fixed, along with
> untestable-by-construction fixtures (a `0,0` cell cannot catch a layer/day transposition) and a
> stale "identity, not equality" test whose newer value also differed in content.
>
> This is the same lesson as finding 3, one layer up: **a test suite's own mutations are chosen by
> the person who wrote the blind spot.** The 26 mutations that now die were almost all somebody
> else's idea.

### 13. README §5 fails `esphome config` two independent ways
`packages/dhw_demand_detector.yaml`, `README.md` — **CONFIRMED by execution, P1 → P2**

Run with `.venv/bin/esphome` 2026.7.3, validation only:

```
[flow_max_stale_seconds] is an invalid option for [dhw_demand]. Did you mean
[droplet_max_stale_seconds], [pump_on_demand_max_stale_seconds], [flow_latch_seconds]?
```

`alpha_hwr_pairing.yaml:53` pins `@v0.15.0` and README §5 declares no `external_components` of its
own, so that pin is the sole source while the `@main` package sets three keys it does not know.
**ESPHome does not dedupe** — `loader.py:239` inserts a meta-path finder per entry and the last
merged entry wins — so a top-level declaration would override it; §5 supplies none, which is why it
breaks while §1–§4 validate clean.

*Bonus, found during verification:* §5 is broken a **second** way, independent of pins. It loads
`alpha_hwr_controls.yaml`, which references sensor ID `motor_speed` (line 546), while the same
recipe renames that sensor to `motor_speed_sensor` → `Couldn't find ID 'motor_speed'`. Both need
fixing.

*Downgraded* because it is 1 of 5 recipes, aborts loudly with a self-explanatory error, and
`bump_version.sh` heals the pin at the next tag — though it recurs on the first new key after every
release, and no CI job runs `esphome config`.

> **Both halves resolved in `dbb9335`**; re-verified by execution (`esphome config` 2026.7.3) on
> the §5 recipe assembled in full, which now validates. §5 carries its own `external_components`
> at `@main`, overriding the package's release pin, and no longer renames the rpm sensor.
>
> **The recurrence, however, was not fixed** — and that was the part with teeth. The recipe broke
> because nothing machine-checked it: the four committed examples are validated in CI, and §5 was
> the one combination (paired pump + control UI + `dhw_demand`) with no file behind it.
> `hwr-pump-dhw-example.yaml` is that file, release-pinned like its siblings, added to the CI
> validation loop and to `bump_version.sh`'s pin list. Confirmed it catches the original bonus
> bug: reintroducing the `rpm: id: motor_speed_sensor` rename fails with `Couldn't find ID
> 'motor_speed'`.
>
> Being release-pinned, it covers the ID-collision class but **not** the pin-mismatch class — it
> is internally consistent by construction, so it cannot reproduce the `@main`-package against
> release-pinned-component failure at all. That half is addressed by the README change below, not
> by this file.
>
> *Correction to the finding's framing:* §1–§3 do **not** "validate clean" for the reason implied,
> and the fix is not deferrable. Each loads an `@main` package whose self-declared
> `external_components` still points at `@v0.15.0` — the same mismatch §4 and §5 patch around.
> They validate only because the packages they load never *set* a post-release key; an added
> optional key with a default breaks nothing merely by existing. So they are **not** "one new key
> away" — the key already exists. `data_timeout` landed after v0.15.0 and is documented as
> user-settable in `docs/configuration.md`, so a user following that page hits the failure today.
> Verified by execution: §1 as previously written rejects `data_timeout` as an invalid option, and
> accepts it once the `@main` override is added. §1–§3 now carry that override.

---

## P3 — real but not worth prioritising

- **Cold cache hands out slot 0** (`schedule_service.cpp:1264`) — **REFUTED as P1 → P3 latent.**
  The C++ mechanism is exactly as described and a destructive write settles ACCEPTED. But every
  entity in the editor package is `internal: true` (27 occurrences) and the button blocks carry no
  `id:`, so **the button is unpressable in every shipped and example config**. The shipped Quick Run
  path calls the *service* → slot `-1` → the protected auto-resolve path; probe confirms the
  vacation survives.
  > **Two reviewers "independently" found this. They did not.** Both restate the same three
  > pre-existing comments and both stop exactly where the comments stop — and reachability, which
  > the comments omit, is what both got wrong. I reported it as convergent evidence; it was one
  > observation counted twice.
  > **Fix belongs in neither place I first suggested:** change the cold-cache `return 0` to
  > `return -1` at `schedule_service.cpp:1264`. Guarding the operation layer would break
  > `submit_clear_single_event`, which legitimately passes an explicit slot with a cold cache.
- **35-slot fallback on an uncached overview** (`schedule_service.h:488`) — **REFUTED as P1 → P3.**
  A ≤10 s post-reconnect window, not a standing condition: `poll_state_async()` fires 500 ms into
  every 10 s tick while the boot single-event read sits ≥10 s down a timer chain. With the overview
  warm, the 5-slot scan completes in **230 ms** and settles accepted. The 90 s cost is real when it
  occurs, and the repo already knew — `tests/test_write_operations.cpp:84-88` warns that 35 × 3 s
  outruns the 60 s watchdog, which is why `WATCHDOG_REFRESH_EVENTS_MS` is 120 s.
  *Residual fix:* `run_refresh_single_events_` lacks the `ensure_overview_()` precondition every
  sibling operation has.
- **`SUBSCRIBING` has no failure exit** (`ble_connection_manager.cpp:189`) — **REFUTED as P1 → P3.**
  The "stale GATT cache" trigger is essentially impossible here: `CONFIG_BT_GATTC_CACHE_NVS_FLASH`
  is unset, so the cache never reaches NVS. The credible trigger is ESPHome's
  `parse_characteristics()` setting `parsed = true` before its loop. Not permanent either — the
  component sets a 4 s supervision timeout on a link that is idle in this state. Not silent — the
  ladder reports Unreachable ~20 s later. *New:* `Session::on_error()` and `Session::reset()` have
  **no caller anywhere**, so the documented ERROR state is unreachable.
- **Two genuine "one write path" bypasses** — `TimeService::set_clock_async` and
  `ControlService::enable/disable_remote_mode`, both **P3**. The §8.4 wire-primitive carve-out
  excuses neither (neither appears anywhere in the 1596 lines of `write_operation_service.cpp`),
  but both are single-step constant-frame writes with no read-modify-write, so neither can fuse
  stale cache — the failure mode §8.4 exists to prevent. Both already carry confirms the finder did
  not credit. The real gap is narrower: no serialization, no `op_id`, no `write_settled` event.
  *Strongest evidence of accidental omission, which the finder missed:* `enable_remote()`/
  `disable_remote()` sit at `alpha_hwr.h:659-666` immediately below `pump_start`/`pump_stop`/
  `set_control_mode`, which all correctly submit to `write_op_service_`.

  **Closed.** Remote mode moved to `submit_set_remote_mode()` in `e0ef766`, the day after this
  report; the clock is now `SET_CLOCK`. Two claims here were wrong, and both were wrong in the
  same direction — toward "P3, architectural tidiness":

  - *"Both already carry confirms the finder did not credit."* `set_clock_async` carried no
    confirm of any kind. It called `callback(true)` on the line after the send, unconditionally,
    beneath a comment promising a verification read that does not exist. The finder was right and
    the refutation was not.
  - *"The real gap is narrower: no serialization, no `op_id`, no `write_settled` event."* The real
    gap was a lie reaching a user-facing sensor. That `true` stamps "Last Clock Sync" and arms the
    24-hour suppression timer, so a write that never left the node showed a fresh timestamp and
    then declined to retry for a day, with the pump running its schedule off whatever clock it
    held. Nothing about "single-step constant-frame write, cannot fuse stale cache" — true as far
    as it goes — bears on that.

  Both errors came from reading the *frame construction* and stopping there. The failure was three
  lines further down.

  One thing the fix turned up that neither pass saw: making the callback honest is not sufficient
  on its own. `update()` runs every 10 s and only stamped the throttle on success, so an honest
  failure would have produced a clock write every 10 seconds forever. The retry timer is stamped at
  submission now, with a 15-minute interval after a sync that did not confirm.
- **Two alleged bypasses are dead code** — the schedule layer writes and `set_state` are referenced
  only from **commented-out** YAML (`packages/alpha_hwr_schedule.yaml:155,158`;
  `hwr-pump-example.yaml:98,103`); `ScheduleService::clear_entry` has zero callers.
  `set_state` is `protected`, not public, and was not "left behind" — the maintainer diagnosed it
  by name *in* the #92 commit while writing its replacement (`schedule_service.h:255-266`).
  Still worth acting on: `schedule_service.h:342` tells the next developer to "Use
  `write_entries_async()` for proper transaction handling" — steering them at the one method that
  omits the mandatory commit and always returns true. **The fix is deletion, not repair.**
- **`is_frame_start` is applied to continuation chunks** (`transport.cpp:206`) — **new, found
  during verification.** A mid-frame fragment beginning with 0x24/0x27 restarts reassembly and
  destroys the frame in flight. Observed 8 times in the repo's own btsnoop captures — ~0.02% frame
  loss from a real pump. Harmless today because CRC rejects the wreckage.
- **`publish_schedule_hash()` is ungated** (`alpha_hwr.h:1163`) — 6 text-sensor publishes with no
  change gate while every neighbouring path gates. Direct AGENTS §4 / #127 violation.
- **DST readback off by 3600 s** — `local_unix_to_utc` resolves the offset from the local value, so
  `set_single_event`/`set_vacation` settle REJECTED though the pump is correct.
- **`Makefile` has no header prerequisites** on multi-source targets — editing
  `write_operation_service.h` gives `make: Nothing to be done` and a stale binary. Verified.
- **Every test write callback hardcodes `return true`**, making `transport.cpp`'s send-failure
  branch unreachable though production returns false on any GATT error.
- **`clear_single_event` accepts slots 0–99** (`api_bridge.cpp:330`) against a ≤35-slot table. The
  only unvalidated argument that reaches the wire — every other service is range-checked or
  whitelisted.

  > *(The citation has drifted: the check is at `api_bridge.cpp:373` now. Line 330 is inside
  > `on_set_schedule_entry`.)*
  >
  > **Half stale, half right, and it took a skeptic pass to separate them.**
  >
  > **Stale half.** `618ca70`, committed 15 seconds before this report, added a device bound to
  > `run_single_event_()` — after `ensure_overview_()`, so it compares against the count the pump
  > reports rather than the 35-slot fallback. The bullet's operative claim, "reaches the wire," was
  > already false when it was written.
  >
  > **Right half.** The first draft of this note argued the bridge's 0–99 needed nothing because
  > it *is* the protocol envelope, citing `schedule_service.h:133,150`. That justification was bad:
  > those are two comments from one commit citing themselves, and the only capture evidence in the
  > repo (`transport.cpp:505`) says 900–904 — this pump's five slots — while the tests call 35 "the
  > protocol maximum". Nothing sourced 999.
  >
  > The conclusion survives on a different footing, one that is checkable here: the SubID is
  > 900 + slot, and the weekly schedule's layer records occupy 1000–1004 (`schedule_service.cpp:85`
  > and its siblings). Slot 100 addresses layer 0, not a single event. So 0–99 is the single-event
  > SubID space because 1000 is spoken for — now named `SINGLE_EVENT_SLOT_LIMIT` with that
  > reasoning attached, rather than left as a bare comment.
  >
  > **And the note's dichotomy was false.** It treated an envelope check as an *alternative* to the
  > device bound. `run_schedule_entry_()` already does both, ten lines away: arguments validated
  > INVALID before `ensure_overview_()`, device work after. Deferring everything had a real cost the
  > draft did not account for — with the link down, an impossible slot settled
  > `"overview not readable"`, blaming the link for an argument that could never have been right.
  > The envelope check now runs first, and its ordering is itself mutation-tested.
  >
  > One limit, deliberate: a slot this *pump* lacks but the protocol allows (50 on a 5-slot pump)
  > still reports the overview failure when the link is down. The count comes from the pump, so with
  > the pump unreachable the honest answer is that the write was not attempted — not a range claim
  > against a number never read. Pinned by its own test.
  >
  > **The bound also moved** out of the explicit-slot branch and into `write_single_event_()`, where
  > all three resolutions funnel. The first draft claimed the old placement covered "any future
  > caller"; it did not — it sat in one of two branches. The auto-slot branch is safe today only
  > indirectly, because `read_single_events_async()` cannot put an out-of-range index in the cache
  > for `find_vacation_slot()` to return. That property now has a test of its own.
  >
  > **Test cover was the rest of it.** The bound shipped with none, and neither way of getting it
  > wrong is visible against the 35-slot simulator: hardcoding 35 accepts slot 10 on this pump, and
  > `<=` for `<` accepts slot == max. Seven tests now model a 5-slot pump, with three mutations
  > pinning both failure modes and the envelope's ordering.
- **Three example YAMLs ship a working all-`A` API key and `test-ota` password.** The MAC gets a
  `# ← CHANGE THIS` marker and WiFi is step 2; the encryption key and OTA password are marked
  nowhere and appear in no setup step.
- **Pairing auto-accept is unconditional** (`ble_connection_manager.cpp:566, 601`) — no address
  check, no bond check, not gated on `enable_pairing`, with `ESP_IO_CAP_NONE`. Hardening note, not
  a live vulnerability.
- **`pump_mode_select` declared by two packages** — fatal ID redefinition, documented as merely
  "duplicate controls".

  > **Confirmed fatal, and there are three collisions rather than one.** Reproduced by validating
  > a config that includes both packages. In the order ESPHome reports them:
  >
  > 1. `Duplicate switch entity with name 'Schedule Enabled'`
  > 2. `Duplicate select entity with name 'Pump Control Mode'`
  > 3. `ID pump_mode_select redefined!`
  >
  > Two conflicting entities produce three errors: the switch contributes one, the select two.
  >
  > The ID redefinition this item is named for is real but reports *last* — it only surfaces once
  > both name collisions are resolved, verified by renaming them in a scratch copy. So anyone
  > searching the tree for the error the audit names would not find it, and anyone renaming their
  > way out of the first two still fails on the third. "Avoid combining both unless you want
  > duplicate controls" understated it: you do not get duplicate controls, you get a config that
  > does not build.
  >
  > *A first draft of this note went further and said no rename composes them. A skeptic pass
  > falsified that in one command — rename all three and the config validates. What is true is
  > that doing so means forking a package to run two overlapping sets of the same controls, which
  > is an argument, not an impossibility. Fixed as documentation on that basis.*
  >
  > **The larger find is what made this invisible**, and it is not limited to this package.
  > `alpha_hwr_schedule.yaml` had no working-tree check of any kind: `tests/ci-compile.yaml` loads
  > `alpha_hwr_controls.yaml`, and the one example that loads the schedule package pins `@vX.Y.Z`,
  > so CI validated the last *release* of it. The same was true of `alpha_hwr_base.yaml` — the
  > package README.md offers as the starting point — which is a parallel copy of
  > `alpha_hwr_pairing.yaml` rather than an include and so cannot share a config either. The
  > skeptic caught that one: the first draft closed the hole for the schedule UI while claiming to
  > have closed it "for everything else", leaving an equivalent instance open. Both are covered now.
  >
  > The schedule UI is compiled as well as validated, because `esphome config` does not compile
  > lambda bodies. *The first draft justified that with a claim the skeptic refuted flatly:* that
  > `ControlMode::AUTO_ADAPT`, `AUTO_ADAPT_RADIATOR`, `AUTO_ADAPT_UNDERFLOOR` and
  > `AUTO_ADAPT_COMBINED` are named "by nothing else in the tree". They are named throughout
  > compiled C++ — `control_service.cpp`'s switches, `mode_to_string()`, the `MODES[]` table, and
  > the host suite. The true and narrower statement is that this package holds the only *YAML
  > lambda* naming them, so renaming an enumerator and letting the compiler find the callers
  > repairs the component and `alpha_hwr_controls.yaml` while leaving this package broken. That
  > still justifies the build, but it is a smaller claim than the one first written down.
- **`time_id` is load-bearing but undocumented** — without it the pump clock never syncs, so
  schedule windows run on a drifting RTC.

## P4

- **`parse_frame` decodes bytes the CRC never covered** (`frame_parser.cpp:61`) — **REFUTED as P1
  → P4.** The code shape is exactly as described, but the trigger is not reachable from a real
  pump. The skeptic replayed the repo's own btsnoop captures — **36,324 notifications, 17,545
  dispatches — zero over-long with a valid CRC.** Every GENI notification is ≤20 bytes despite a
  negotiated ATT MTU of 65, so the chunking is the pump's firmware behaviour, not an MTU floor. It
  requires one ATT notification containing a frame plus trailing bytes, which the pump never sends.
  And the only actor who could do that can already inject arbitrary frames on the command-response
  path, which has no CRC validation at all — strictly easier. Cheap fix, but hygiene.

## 14. `read_entries()` is a documented trap: use-after-scope when called as the docs show
`schedule_service.cpp:454-514` — **CONFIRMED by sanitizer, P0 → P2**

The lambda captures the caller's raw `std::vector<ScheduleEntry>*` by value and writes through it
(`entries->clear()`, `entries->push_back()`) from the transport callback, which fires up to 3 s
after `read_entries` has already returned `true`. Probe (`skeptic_readentries.cpp`, ASan with
`-fsanitize-address-use-after-scope`) traps cleanly on the real path:

```
Transport::on_notification (transport.cpp:252)
  -> try_dispatch_response (transport.cpp:537)
    -> read_entries lambda (schedule_service.cpp:496)
      -> vector::clear()
ERROR: AddressSanitizer: stack-use-after-scope
```

**Downgraded from P0 because nothing calls it**, and — the surprising part — **the broken default
is what protects the naive caller.** `layer` defaults to `-1`, and unlike `read_entries_async`
(which handles `-1` as "read all layers", `schedule_service.cpp:528`), the sync version rejects it
at the `layer < 0 || layer > 4` guard *before queuing any wire traffic*. Probe scenario A confirms
`read_entries(&out)` returns `false` having done nothing. The use-after-scope needs an **explicit**
layer 0-4.

Which is exactly what the documentation tells users to write. `packages/alpha_hwr_schedule.yaml:135`
carries the recipe, currently commented out:

```cpp
std::vector<esphome::alpha_hwr::ScheduleEntry> entries;
bool result = id(pump).read_schedule_entries(&entries, 0);   // explicit layer 0
```

In an ESPHome YAML lambda that local vector dies when the lambda returns, milliseconds before the
pump answers. A user who uncomments the documented recipe gets a guaranteed write into a dead stack
frame — on a device with no MMU and no sanitizer, so it corrupts silently rather than trapping.

The other documented example is worse-formed but harmless: `schedule_service.h:130` shows
`std::vector<ScheduleEntry> entries = id(pump).read_schedule_entries();`, which **does not compile**
— two errors (no argument for the non-defaulted pointer parameter, and `bool` is not convertible to
`vector`).

*Fix: delete it.* `read_entries_async` supersedes it, handles `-1` correctly, and owns its vector.
The sync version is uncalled, its default can never succeed, and both of its documented examples are
wrong. Same disposition as `write_entries_async` above — deletion, not repair — and remove the two
doc recipes with it.

## Unverified

- The P2/P3 tail not sent for refutation: watchdog budgets not derived from wire timelines,
  `write_temp_range_config` echoing an unvalidated cache, the midnight-crossing upload codec gap,
  the §9 step 6/7 documentation gaps.

## Leads refuted

- `register_response_handler` has **zero callers** repo-wide — the `pending_handlers_`
  erase-during-iteration hazard is unreachable, and `check_timeouts(2000)` is consequently a no-op.
- `callback()` before `pop_front()` — no callback can reach `Transport::reset()`, and
  `std::deque::push_back` preserves element references. 40 chained re-entrant completions,
  sanitizer-clean.
- `control_service` unguarded commit timers — all terminate in a session-ready or cache-valid guard.
- `flow_buf_` uninitialised (cppcheck) — `setup()` seeds all 30 slots to NaN, every read
  `isnan`-guarded.
- `format_codes` string churn — result is `"None"`, inside SSO.
- `packet_raw[256]` stack risk — the notification path is not recursive and runs on the main loop
  task.
- Codegen ↔ C++ — clean. All 44 + 26 setters resolve; no dead keys; the
  `droplet_max_stale_seconds` → `flow_max_stale_seconds` rename is complete; defaults match §11.7.
- AGENTS §8.4 rules 1, 2 and 4 all hold under fuzzing.
- `schedule_codec.cpp`, `codec.cpp`, `auth.cpp` — clean; all four hardcoded `AUTH_*` CRCs verified.

## Tooling gaps

> **Status, 2026-08-14.** All six ranked items below have landed: `.github/workflows/test.yml` now
> runs sanitizers, `esphome config` on every example, cppcheck via the fixed `lint.sh --strict`,
> both compilers, ruff + mypy, and a syntax/`op_id` check on the card — plus a mutation check and
> the full ESP32-C3 firmware build. One gap the audit did not name has also been closed: `esphome
> compile` was the **only** thing in the toolchain that compiled `dhw_demand.cpp`, so the 538 lines
> holding the branch selection behind finding 9, the flow latch and every publish were unreachable
> by the unit suite, cppcheck and the mutation check alike. The component is now host-compiled and
> driven by `tests/test_dhw_demand_component.cpp`. `clang-tidy` and `black` remain absent.
>
> The same gap turned out to cover four more files, and they are closed too: `auth.cpp`,
> `sensor_publisher.cpp`, `telemetry_service.cpp` and `device_info_service.cpp` compiled against
> the existing mocks unmodified, so each needed only a Makefile target and a test. Host-compiling
> `device_info_service.cpp` for the first time immediately surfaced three dead-code defects in it.
>
> Still firmware-build-only for real reasons: `alpha_hwr.cpp`, `ble_connection_manager.cpp` and
> `api_bridge.cpp` need ESP-IDF or the API SDK. `time_service.cpp` is the one remaining file that
> could be host-compiled but is not — it wants a `real_time_clock.h` mock first.

**`tools/lint.sh --strict` swallows its own output.** `set -euo pipefail` plus `--error-exitcode=1`
kills the script at the `OUTPUT=$(cppcheck …)` assignment the moment cppcheck finds anything, so
the mode meant to enforce quality prints its header and exits without showing a finding. Fix:
`OUTPUT=$(cppcheck … || true)`.

**CI runs one command on one compiler.** Ranked by defect class caught per CI-minute:

1. **Sanitizers on Linux** — LeakSanitizer works there and would have caught finding 2 outright.
   Near-zero marginal cost; the suite already builds.
2. **`esphome config`** on the example YAMLs — would have caught finding 13 *and* its bonus, and
   catches every future codegen/schema break. `.venv/bin/esphome` is already present.
3. **cppcheck** via the fixed `lint.sh --strict`.
4. **A second compiler** — `make test-clang` already exists.
5. **`ruff` + `mypy`** — AGENTS §3 requires them; neither runs anywhere.
6. **Anything at all for the JS card** — 1757 lines, zero coverage, and the source of the two
   highest-confidence findings here.

Also absent: `clang-tidy` has a config but needs a `compile_commands.json` nothing generates;
`black` is required by AGENTS §3 and not installed.

## Coverage and method notes

**Reviewed:** all of `components/`, `tests/` and its mocks, the Lovelace card, both `__init__.py`
schemas, `packages/*.yaml`, the example configs, `tools/write_bench.py`, `.github/workflows/`.

**Not host-testable:** `ble_connection_manager.cpp` depends on ESP-IDF headers, so findings 8 and
the `SUBSCRIBING` entry rest on reading plus the vendored ESPHome 2026.7.3 sources — the skeptic
said so rather than fabricating a probe. The real `session.cpp` *was* isolated and compiled against
the mocks.

**Baseline:** `make test` 21/21 green, zero warnings; ASan+UBSan over the suite clean; cppcheck
0 errors / 12 warnings / 29 performance; `ruff` and `node --check` clean; `mypy` 2 errors. The tree
was left clean — no source file was modified by this review.

**What verification changed:** 12 of 14 severities moved, 9 of them down. Three proposed fixes were
wrong and are corrected inline (findings 2, 5, 9, and the slot-0 entry). One finding I reported as
corroborated by two independent reviewers turned out to be one observation counted twice. Two new
defects were found *by the skeptics* rather than the finders (the continuation-chunk frame loss,
and `set_state_async`'s cache poisoning). The finder pass alone would have been actively
misleading on priority.
