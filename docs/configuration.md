# Configuration

## `api:` requirements

The component's programmatic write services and the
`esphome.alpha_hwr_write_settled` event (see
[programmatic-interface.md](programmatic-interface.md)) require two flags on
the ESPHome `api:` component:

```yaml
api:
  custom_services: true
  homeassistant_services: true
```

All shipped packages and examples set these. Without them the component still
compiles and the entities work, but no services are registered and no settle
events fire.

> Naming note: the existing `reconnect_settle_time` option below is unrelated
> to the `write_settled` event — it is a BLE reconnect hold-off.

## alpha_hwr Component

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `ble_client_id` | string | **required** | BLE client ID for pump connection |
| `time_id` | ID | none | A `time:` component to sync the pump's clock from. Optional in the schema, but see below — without it the pump's clock is never set. |
| `enable_pairing` | boolean | `false` | Enable BLE pairing for control and enhanced telemetry |
| `reconnect_settle_time` | time | `2s` | Delay after disconnect before reconnecting |
| `control_state_poll_interval` | time | `30s` | Interval for periodic control state polling. Set to `0s` to disable. |
| `data_timeout` | time | `60s` | Tear the BLE link down after this long with no data from the pump, so the normal reconnect runs. Set to `0s` to disable. |

### `time_id`

Optional in the schema, and the thing most worth checking if pump schedules fire
at the wrong time.

The pump keeps its own real-time clock and runs schedule windows off it. Nothing
else corrects that clock, so `time_id` is what keeps it honest: point it at a
`time:` component and the component writes the pump's clock at boot and daily
thereafter, retrying every 15 minutes until one confirms.

Leave it out and no clock write ever happens. The pump keeps running schedules
off an RTC that nobody is setting, and the drift is silent — telemetry, control
and the schedule editor all keep working, so nothing looks wrong until a window
opens at the wrong hour. A node in that state says so at startup on the serial
console:

```
[W][alpha_hwr]: No time_id configured - the pump clock will never be synced
[W][alpha_hwr]:   The pump runs schedule windows off its own RTC, which drifts
[W][alpha_hwr]:   Add `time_id:` pointing at a time component to enable syncing
```

That line is serial-only: component setup runs long before the API server
accepts a log client, and ESPHome keeps no backlog for late subscribers. What an
`esphome logs` session sees is this instead, repeated at most hourly once the
pump link is up:

```
[W][alpha_hwr]: Pump clock is not being synced: no time_id is configured
[W][alpha_hwr]:   Schedule windows run on the pump's own RTC, which drifts
```

Both entry packages — `alpha_hwr_base.yaml` and `alpha_hwr_pairing.yaml` —
already wire this up, so this applies to configs that declare `alpha_hwr:` by
hand:

```yaml
time:
  - platform: homeassistant
    id: hwr_time

alpha_hwr:
  ble_client_id: hwr_pump_client
  time_id: hwr_time
```

Any `time:` platform works — `homeassistant`, `sntp`, or an RTC. The component
waits for the source to actually be set before writing (it requires a year of
2021 or later), so a node that boots before its time source answers syncs on a
later poll rather than writing a bogus clock. That wait is on the 10-second poll,
not the 15-minute retry: the retry interval applies once a write has been
attempted and did not confirm.

A configured `time_id` whose source never answers fails the same way and is the
likelier case, since both entry packages set the option: a `homeassistant` time
platform on a node that cannot reach Home Assistant looks configured and never
produces a clock. That is reported too, after a 15-minute grace window so a node
that is merely still booting is not accused:

```
[W][alpha_hwr]: Pump clock is not being synced: its configured time source has not answered
[W][alpha_hwr]:   Schedule windows run on the pump's own RTC, which drifts
```

Both repeat at most hourly, and both repeat from the 10-second poll, which runs
only while the pump link is up — a node that has never connected reports on
serial alone.

What this does *not* catch: `settimeofday()` is one way, so a time source that
answers once and then becomes unreachable leaves the system clock running and
valid. The pump keeps being written, from an ESP RTC that nobody is correcting,
and no warning fires. What is detected is a clock that was never set, not one
that stopped being kept.

If you use `alpha_hwr_pairing.yaml`, the **Last Clock Sync** text sensor reports
when a write was last confirmed by the pump and **Clock Drift** reports how far
off the pump was when it was found; the two together are the way to check this
is working. `alpha_hwr_base.yaml` declares neither, so on that package and on
hand-written blocks the log is the only signal.

### `enable_pairing`

Pairing is initiated by the pump, not by this node. On an unbonded connection
the node stays silent and waits for the pump's security request, because a
pairing request sent from this side to an unbonded pump comes back "Pairing Not
Supported" and loses the pump's own request in the process.

That works whenever the pump is willing to pair. It has one failure mode, and it
is not recoverable over the air:

> **Clearing this node's bond without clearing the pump's requires physical
> access to the pump.** `ble_client.remove_bond`, an NVS erase and a re-flash
> that loses NVS all leave the pump holding a bond for a peer that no longer
> has one. The pump then sees an unencrypted stranger at a bonded address,
> sends no security request, and drops the link — every time, indefinitely.
> Do not do it remotely.

The recovery is to put the pump back into Bluetooth pairing mode by hand. That
is more than one button press, and the steps are not guessable from the pump —
this is the procedure reported by @jfriend00, who has run it repeatedly on their
own pump (see [#229](https://github.com/eman/esphome-alpha-hwr/pull/229)):

1. Go to the pump physically.
2. **Stop this node from connecting first** — power it down, or disable the BLE
   client. The pump accepts only one BLE connection at a time, so a node
   reconnecting every few seconds will keep the next step from working. This is
   exactly what a node in this fault state is doing.
3. Connect the Grundfos GO app to the pump.
4. Unlock the pump's front panel from the app. The panel re-locks itself on its
   own, so do this immediately before the next steps rather than in advance.
5. Disconnect the GO app, again because the pump holds one connection at a time.
6. Press the pairing button on the front panel and wait about 15 seconds to see
   whether a link is established.
7. If nothing happens, press it again. It commonly takes several attempts.

Then let the node reconnect. **Set `enable_pairing: true` before any of this**,
or the pump's offer goes nowhere: with it false this component configures no IO
capability, no bonding requirement and no key distribution, so nothing is set up
to complete a bond. (ESPHome's own BLE client answers the pump's request
regardless — this node cannot decline it — but answering is not bonding.)

Two caveats on the procedure. It is one owner's routine on one pump, not
something this project has verified across models, and the panel-unlock step in
particular may be specific to how that pump is configured. And it is a re-pair,
not a reset of the pump's own bond table: clearing a bond *at the pump* is a
different operation that nobody here has needed, and as far as is known it takes
a full pump reset.

The node cannot tell that state apart from a pump that has simply never been put
into pairing mode — in both cases the evidence is an absence, no security request
— and it does not try to, because the remedy is the same. After three
consecutive connections that open with no bond, exchange no security and carry
no data, it says so: a `WARN` naming both possibilities and the remedy, repeated
about once a minute, and **Pump Link Fault** reading `Pump not accepting
pairing`. Without that the fault sensor shows `Failed To Establish (0x3e)`,
which points at radio trouble — and the radio is fine; the connections succeed.

Three cycles rather than one, so an ordinary dropped link is not reported as a
pairing problem — and three specific cycles. A connection that carried data is
not counted, so an unbonded node running read-only telemetry (the default, since
`enable_pairing` is `false`) does not accumulate them on its ordinary
reconnects. Neither is a link the node dropped itself: a `data_timeout` recycle
looks identical in every other respect, and blaming pairing for a pump that is
simply not answering would replace a true diagnosis with a false one. Neither,
finally, is a link the radio lost — a supervision timeout, a connection that
failed to establish — because the claim being made is about the pump's
behaviour, and those are not evidence about it in either direction.

What remains is a judgement, not a certainty. The node sees an absence, and it
reports the two things that absence usually means. A third cause with the same
signature is possible — anything that repeatedly drops an opened link before
data flows — so read the message as "the pump is not pairing and here is what
usually explains it", not as a positive identification.

The likeliest such cause is worth naming, because it follows from the pump
holding **one BLE connection at a time**: anything else already talking to it —
the GO app, a phone left connected, a second node — takes the slot this one
needs. If the fault appears while somebody is standing at the pump with the app
open, that is the first thing to rule out.

### `data_timeout`

Nothing on the connect path verifies that the pump is answering. The session is
declared ready a fixed two seconds after notifications are enabled, and not a
single frame has been exchanged by then, so it reaches its ready state whether
or not anything is listening. A link whose writes succeed but whose
notifications never arrive therefore reports itself healthy indefinitely — every
sensor frozen, nothing to trigger a reconnect, because the BLE connection itself
is fine.

`data_timeout` closes that hole with a liveness check: if nothing is received
for this long while connected, the link is dropped and the usual reconnect
takes over. During the reconnect the **Pump Link Fault** sensor reads
`No data from pump (60s)` — held there rather than overwritten by the local
disconnect that caused it — and returns to `None` once data flows again.

The window is timed from connection-open and refreshed on every received
notification. A working link is polled every 10 seconds, so in steady state the
default tolerates five missed poll cycles and acts on the sixth.

This recovers a link that *can* recover. A pump that is permanently deaf will
instead cycle — roughly 60 seconds looking connected, then about 6 seconds
reconnecting — because reaching the ready state still does not require data. If
you see the link status and fault sensors flapping on that cadence, the pump
itself is not answering and reconnecting will not fix it.

Raise `data_timeout` if your setup has long legitimate quiet periods; set it to
`0s` to disable the check entirely.

#### Backoff, and the two diagnostics that go with it

A link that stays deaf is not recycled on the configured cadence forever. Each
recycle that produces no data doubles the window for the next one, up to a
ceiling of one hour, and a notification received once the session is **ready**
resets it to the configured value. (Ready-gated because a deaf pump still
volunteers operation-status notifications of its own accord: resetting on those
frames would clear the window once per session and the backoff would never
engage.) A link that can recover still
recovers on the first or second try; a permanently deaf one drops from roughly
1,300 recycles a day to about 28.

A widened window is not reset by the reconnect either, so it governs the next
connection's opening as well — which matters for reading `link_max_gap` below.

That matters because a recycle is not free: each one re-enters the
encryption-on-open path on a bonded pump, taking one more run at the window
where an encryption request can fail and erase the bond.

The cost is a slower recovery in one case. The window only governs a session
that is *connected but silent* — a pump that loses power drops the link, and the
node reconnects on its own cadence with the window reset — but a pump that stays
connected and mute for over an hour will have grown the window to the ceiling,
and if it starts answering just after a check it waits up to a further hour to
be noticed. Worst case is 60 minutes against roughly 66 seconds before. That is
the trade for bounding the recycle count; set `data_timeout: 0s` to opt out of
the watchdog altogether if it is the wrong one for your setup.

Two optional diagnostic sensors expose the state:

| Entity | Reads |
| --- | --- |
| `link_recycles` | Consecutive recycles with no data in between. `0` on a healthy link. |
| `link_max_gap` | Longest quiet interval since boot, in seconds. |

`link_recycles` is the one to alert on. The Pump Link Fault sensor shows the
reason *during* a reconnect and clears on recovery, so repeated recycles read as
a flap you have to be watching to catch; the counter is a value an automation
can threshold on.

`link_max_gap` exists because the `60s` default is not yet chosen from
observation. The two requirements pull against each other — clear of every gap
that happens routinely, short enough not to sit in the stuck state for long —
and only data from real installations settles it.

It samples the intervals the watchdog is timed over, which is what makes it
usable for that: the clock starts at connection-open, and an interval is closed
by whatever ends it — a notification, a watchdog recycle, or the link dropping.
So the open-to-first-notification interval is included (it is inside the window,
and against a 60s budget it is the tightest case, not the loosest), and so is a
quiet period cut short by a recycle or a drop, which is where the number would
otherwise be silent about exactly the excursions it exists to reveal. Time spent
disconnected is not sampled, since the watchdog is not running then either.

> **A reading is a lower bound on how long the link went quiet**, not a
> measurement of it. When a recycle or a drop ended the interval, how long the
> quiet would have lasted is unknown.
>
> **A high reading does not by itself mean a recycle happened.** The watchdog
> runs against the window currently in force, and the backoff widens that after
> a recycle — so a 90s interval that ended on its own under a widened 120s
> window reads the same as one cut off by a 60s ceiling. `link_recycles` and the
> Pump Link Fault sensor are what distinguish the two; this number alone cannot.

Because the sensor tracks a maximum, expect it to settle at roughly the 10s poll
interval on a healthy link rather than at zero — the first poll cycle of the
first connection puts it there. It is a RAM value and starts over at every boot,
including a reflash; Home Assistant's long-term statistics keep the hourly
maximum indefinitely, so use the statistics graph rather than the current state
when you want the history across reboots.

With `data_timeout: 0s` the sensor still records intervals, but there is no
budget for them to be read against and no recycle to cut them short — it becomes
a plain "longest quiet period on a live link" number.

#### The gap histogram

`link_max_gap` reports the worst interval. It cannot report how *many* there
were, and that is the question the default actually turns on: a budget of `T`
fires once for every quiet interval longer than `T`, so the number of those per
day is the cost of choosing `T`. Eight more optional sensors report it.

| Entity | Reads |
| --- | --- |
| `link_gaps_over_15s` … `link_gaps_over_90s` | Quiet intervals longer than 15/20/30/45/60/90 seconds, since boot |
| `link_gaps_truncated` | Of those intervals, how many were cut short by a recycle or a drop instead of ending on their own |
| `link_watch_time` | Seconds of live link the counts were drawn from |

Each `link_gaps_over_Ns` counter is, exactly, **the number of times a
`data_timeout` of N seconds would have fired** — the comparison is the same one
the watchdog makes. So `link_gaps_over_45s` divided by `link_watch_time` is the
recycle rate a `45s` default would have cost this installation, measured rather
than predicted.

The rungs are chosen against the timings in the section above: 15s and 20s are
one missed poll cycle, 30s and 45s sit clear above the worst case for reaching
first data after a connect (they straddled it at 21.5s and 31.5s when they were
chosen; removing the opening sequence took it to 16.0s, and the rungs are left
where they are so the counts already gathered against them stay comparable), 60s
is the shipped default, and 90s is
above it deliberately — without a rung there the data cannot distinguish an
excursion of 70s (which a longer default would cover) from one lasting minutes
(a genuine link death, which *should* recycle).

`link_watch_time` counts only time the link was up, so a rate computed from it
is per day of connected pump, not per calendar day. It is also the coverage
check: if it is far below the wall-clock length of your run, the counts came
from less observation than you think.

> **Set `data_timeout` above the top rung before you start, or the counters
> lie.** The watchdog closes a quiet interval as soon as the budget expires, so
> under the `60s` default nothing can ever be recorded above 60s — the `90s`
> counter reads zero however badly the pump behaves, and the `60s` counter is
> really "how many recycles", not "how many gaps that long". The backoff makes
> it worse by widening the budget after each recycle, so the cutoff moves during
> the run. Use `data_timeout: 600s` for a measurement run. The component logs a
> warning at boot if the histogram is configured under a budget too small for
> it, and `link_gaps_truncated` is how you check afterwards: it counts the
> intervals that were cut off rather than observed.

Unlike `link_max_gap` these are `total_increasing`, so Home Assistant's
long-term statistics recognise the reset at each boot and keep accumulating —
which is what makes a run measured in weeks survive the OTAs and restarts it
will certainly meet. Read them from the statistics graph, not the current state.

`tools/link_gap_report.py` reads all of this off one or more nodes, pools it,
and prints the recycle rate each candidate default would have cost, with the
decision rule it applies printed alongside the answer.

#### Running a measurement run

The eight entities are **off by default** — they are an instrument for one
decision, not something every install should carry. To take part in a run:

**1. Raise `data_timeout` first.** Set `data_timeout: 600s` on every pump taking
part. The watchdog still recovers a deaf link, just more slowly. Do this
*before* declaring the entities: at the shipped `60s` the top rungs cannot fill,
and the run would produce reassuring zeros whatever the pump did.

**2. Declare the entities.** Uncomment the histogram block in
`packages/alpha_hwr_pairing.yaml`, or name the eight keys in your own
`alpha_hwr:` block. `esphome config` warns if the budget is still too small for
the rungs you declared.

> The component also warns at boot, but that runs before the API server is up,
> so it reaches the serial console only — over the air you will not see it. The
> `esphome config` warning is the one to rely on.

**3. Snapshot periodically.** This is not optional, and it is the step most
easily missed: the counters are RAM values that restart at every boot, so a
single read at the end tells you about the current boot and nothing else — and a
run measured in weeks will meet an OTA. `snapshot` appends one record per node
to a log; `report` reconstructs the totals across boots from it, using the same
reset rule Home Assistant applies to a `total_increasing` sensor.

```bash
venv/bin/python tools/link_gap_report.py snapshot \
    --host hwr-pump.local --secrets secrets.yaml
```

The log defaults to `link_gap_log.jsonl` in the working directory (gitignored);
`--log FILE` puts it elsewhere. Repeat `--host` to read several pumps in one
run — a `--key` or `--secrets` binds to every `--host` after it, and one
connection is opened and closed per node, because connection count is what costs
heap on these nodes (issue #127).

**Cadence: every couple of days for the first fortnight, then weekly.** The
reason is the `unsure` column described below — snapshot intervals longer than
the watched time a node has banked could hide a reboot, and early in a run that
is every interval. Once the node has more watched time banked than your snapshot
gap, hidden reboots become impossible and the column stays at zero.

**4. Leave it alone for a few weeks** of ordinary service — not a bench session,
since the point is what normal operation does. The report refuses below 14 days.

**5. Run the report.**

```bash
venv/bin/python tools/link_gap_report.py report --budget 600
```

`--budget` is the `data_timeout` that was in force during the run. It is not
readable over the API, so you supply it, and the report refuses rather than
guesses if any rung you declared sits at or above it.

`report` needs nothing but the log file, so it runs on a plain `python3`;
only `snapshot` needs `aioesphomeapi`, which the esphome venv provides.

#### Reading the report

Five sections. A real run, three weeks on one pump snapshotted every three days,
with one OTA reboot in the middle:

```
COVERAGE
  node                        watched  snapshots  reboots  unsure  truncated
  hwr-pump                     20.86d          8        1       3          2
  pooled                       20.86d
```

**COVERAGE is the "should I believe any of this" section.** `watched` is
live-link time, not calendar time — if it is far below the wall-clock length of
your run, the counts came from less observation than you think. `reboots` is how
many times the counters restarted and were stitched back together. `unsure`
counts snapshot intervals long enough that a reboot could have happened and
climbed back past the previous total unseen, which would silently drop that
session; a nonzero count means the totals are a **lower bound**. `truncated` is
intervals cut short by a recycle or a drop rather than ending on their own —
each one is an observation whose true length is unknown.

```
SURVIVAL  (intervals longer than T, and the rate they arrived at)
      T   pooled  /node-day           hwr-pump
    15s      975    46.7299                975
    20s       72     3.4508                 72
    30s        2     0.0959                  2
    45s        1     0.0479                  1
    60s        0     0.0000                  0
    90s        0     0.0000                  0
```

**SURVIVAL is the measurement.** Each row is how many quiet intervals ran longer
than T — which is exactly how many times a `data_timeout` of T would have fired,
because the counter uses the same comparison the watchdog does. Per-node columns
sit alongside the pooled figure so one bad installation is not hidden by
averaging.

```
BUDGETS  (what each candidate default would have cost)
      T  recycles/day  days between    worst node  verdict
    30s        0.0959          10.4        0.0959  FAIL below the 41s floor
    45s        0.0479          20.9        0.0479  FAIL over the recycle tolerance
    60s        0.0000         never        0.0000  PASS
```

*(excerpt — the real table carries a row for every rung, 15s through 90s)*

**BUDGETS turns that into cost.** `worst node` is the highest per-node rate, not
the pooled one, so a candidate has to be defensible everywhere rather than on
average.

```
RECOMMENDATION
  data_timeout: 60s
  Conservative alternative: 90s
```

**RECOMMENDATION applies a rule the report then prints in full**, so it can be
argued with rather than taken on trust:

- **Floor: T ≥ 41s.** The calculated worst case from connection-open to first
  inbound data is 16.0s (see the sizing note in
  `components/alpha_hwr/link_watchdog.h`), which with 30% margin would give 21s.
  The floor is deliberately above that: it also has to clear the
  missed-poll-cycle rungs, and lowering a recommendation is a policy change this
  measurement has not made yet.
- **Tolerance: under one spurious recycle per installation per 30 days.** Each
  recycle takes another run at the encryption-on-open window that can erase the
  bond (issue #14), which is what makes recycles worth being stingy about.
- **Choose the smallest rung meeting both.** Detection latency is the cost on
  the other side, and these counters cannot measure it — that half is judgement,
  and the report says so rather than dressing it as arithmetic.

If the evidence does not support a recommendation the report says
`INSUFFICIENT EVIDENCE` and lists why: under 14 days of watched link, a
truncation rate high enough that the tail was clipped rather than observed, or a
budget that could not let the declared rungs fill. Those are all properties of
the data. There is deliberately no minimum node count — how many installations
exist is a fact about the world, not about the evidence — so a single pump can
produce a recommendation, and the caveats record that it describes that pump.

**CAVEATS is printed every time and cannot be suppressed.** The one that most
affects how you read BUDGETS: `recycles/day` is an *upper* bound on what a
budget would cost, because the backoff widens the window after the first recycle
of a deaf episode, so repeated excursions do not each cost one. It understates
the benefit for the same reason. The rule budgets the cost, which is the side
that argues against lowering the default.

A measured default that ends up where it started is a real result, not a failed
run: it replaces "this is a constants calculation" with "this is what a month on
real hardware looked like".

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
    name: "Head"
  rpm:
    name: "Motor Speed"
  voltage:
    name: "AC Voltage"
  current:
    name: "Motor Current"
```

## Build Identification

Two diagnostic entities answer "what is actually running on this node?", which
matters when correlating a behavior change with an install (issue #124):

| Entity | Source | Example |
| --- | --- | --- |
| `Component Version` | `${component_version}` substitution in the package, published at boot | `0.13.0` |
| `Component Build` | `component_build:` on `alpha_hwr:` | `v0.13.0-30-g066640c-dirty (built 2026-07-27 20:08:58 -0700)` |

`Component Version` is the **release** version — it changes only when a release
is cut, so every build between two releases reports the same value. Use it to
answer "which release is this?".

`Component Build` identifies the **build**. The revision is `git describe` of
the component's source tree, resolved at compile time: ESPHome clones
`github://` sources with git and a `type: local` source is your working tree, so
both resolve (`-dirty` marks uncommitted changes; `unknown` means git wasn't
available, e.g. a tarball install). The firmware build timestamp is appended, so
two flashes of the same revision are still distinguishable. Include this value
in bug reports.

## Node Health Diagnostics

Both packages expose the ESP32's runtime heap through ESPHome's `debug`
component, polled every 60 s (issue #127):

| Entity | What it answers |
| --- | --- |
| `Free Heap` | Headroom right now |
| `Min Free Heap` | The worst moment since boot — the one that survives a spike you weren't watching |
| `Largest Free Block` | Fragmentation's practical effect: a large allocation fails on this, not on `Free Heap` |
| `Heap Fragmentation` | Trend, as a percentage |
| `Reset Reason` | Why the node last rebooted — separates a panic from an OTA or a power cut |

The build's static-RAM figure does not capture any of this: the interesting
allocations happen at runtime with the BLE stack up and the API queueing
outgoing frames.

### Log level and API subscribers

The packages set `logger: level: INFO`. Every log line, like every state change,
is an API frame fanned out to **each** connected subscriber, so a node with Home
Assistant plus an `esphome logs` stream plus a polling script pays each DEBUG
line three times — and the node has run out of heap inside ESPHome's outgoing
API buffer under exactly that load. Raise it deliberately when troubleshooting
(your own config wins over the package):

```yaml
logger:
  level: DEBUG
```

and prefer keeping at most one extra subscriber attached beyond Home Assistant
while you do. Watch `Free Heap` / `Min Free Heap` during long debug sessions.

## Control State Polling

Periodically reads pump control state to detect changes from internal schedules, manual button presses, or external apps. Keeps component state synchronized with pump reality.

**Default:** 30 seconds polling  
**Disable:** Set to `0s` if you guarantee exclusive component control  
**Customize:** Use any time interval (e.g., `60s`, `15s`)

Polling is non-blocking and doesn't impact other component operations. Failures are logged but don't break anything.

It is also what keeps the `pump_run_state` / `schedule_stalled` entities honest
and drives the stalled-schedule repair (see
[schedule-management.md](schedule-management.md#the-stalled-schedule-and-how-it-repairs-itself)).
With polling set to `0s`, run state is only re-read after the component's own
writes, so a stall introduced by the Grundfos GO app is not noticed until the
next reconnect.

## dhw_demand Component

Infers hot-water draws from pump and tank telemetry. Independent of `alpha_hwr`;
see [Architecture](architecture.md#dhw-demand-detection) for how the detection
works. Every key is optional — the detector uses whatever sensors it is given and
degrades gracefully as inputs drop out (an unavailable sensor reads NaN and the
rule that needed it declines rather than guessing).

> **Breaking change in this release (issue #149).** The pump-on hydraulic vote
> tier was replaced by a direct measurement, and nine keys went with it:
> `inlet_pressure`, `pump_power`, `pump_head_rate`,
> `inlet_pressure_transient_threshold`, `inlet_pressure_demand_floor`,
> `pump_flow_collapse_threshold`, `motor_current_spike_threshold`,
> `pump_power_spike_threshold` and `pump_head_rate_threshold`.
>
> A config that still sets one **fails at `esphome config` time** with a message
> naming the replacement — deliberately, rather than being accepted and ignored,
> because config that validates but does nothing is worse than config that
> breaks. **To migrate: delete them.** Nothing replaces them one-for-one; the
> new keys are listed under Thresholds below and all four have working defaults.
>
> Make sure `pump_flow` and `motor_speed` *are* wired. They are now what pump-on
> detection runs on. Without them the detector still works while the pump is off
> and reports `pump_on_uncertain` whenever it is running.

### Output sensors

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `demand` | binary_sensor | — | Whether a draw is in progress |
| `confidence` | sensor | — | Detection confidence, 0–100 % |
| `demand_level` | sensor | — | Estimated draw intensity, 0.0–1.0 |
| `session_duration` | sensor | — | Live duration of the current draw, seconds |
| `detection_method` | text_sensor | — | Which rule fired (e.g. `deterministic_flow`) |

`confidence` and `demand_level` answer different questions: confidence is how sure
the detector is that a draw is happening, `demand_level` is how large it looks.
The two move independently — a small draw can be measured with high confidence.
Intensity is `min(1.0, GPM / 2.5)` in both pump regimes: household flow while the
pump is off, and *computed* demand (`flow − pump_flow`) while it is on. It is not
a flow rate and should not be read as one. While `demand_release_seconds` is
latching, the last live value is republished rather than 0.0.

Confidence on the pump-on branch rises with how far the measured draw clears the
threshold rather than with a count of agreeing signals:
`min(0.90, 0.60 + 0.30 × margin)`. The 0.90 cap reflects that the measurement is
a difference of two instruments, not a direct reading of the tap.

All five outputs publish **on change only** (issue #129), not once per
`update_interval`. They are step-valued — `detection_method` names the rule that
fired, `confidence` and `demand_level` are flat between transitions, and
`session_duration` is `0` while nothing is drawing — so a per-tick republish sent
an API state frame to every subscriber carrying no new information. A running
draw is unaffected (`session_duration` moves every tick while a session is open),
and Home Assistant still gets every entity's current state on connect, so this is
invisible except in state traffic. If you need one of these as a liveness
heartbeat, set `force_update: true` on that sensor — it restores the per-tick
publish and also tells Home Assistant's recorder to store every repeat:

```yaml
dhw_demand:
  confidence:
    name: "DHW Detection Confidence"
    force_update: true
```

`detection_method` is a `text_sensor` and has no `force_update`; use `demand` or
the node's own connection state for availability instead.

### Input sensors

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `flow` | sensor id | — | Household hot-water flow (GPM). The ground-truth signal while the pump is off |
| `tank_lower_temp` | sensor id | — | Tank lower temperature (°F); drives the thermal-collapse signal |
| `dhw_charge` | sensor id | — | Tank charge (%); drives the charge-drop signal |
| `motor_speed` | sensor id | — | Pump RPM; selects the pump-on/pump-off branch |
| `motor_current` | sensor id | — | Pump current (A); branch fallback when RPM is absent |
| `pump_flow` | sensor id | — | Pump's own recirculation-loop flow (GPM). Subtracted from `flow` to measure household demand while the pump runs |
| `dhw_in_use` | sensor id | — | The heater's own DHW in-use flag. Applies a +0.05 confidence boost while the pump is off, and — once continuously high for `dhw_in_use_min_seconds` — can declare a pump-on draw on its own as the last-resort recall tier |

### Thresholds

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `pump_off_current_threshold` | float | `0.03` | A — below this the pump counts as off |
| `flow_threshold` | float | `0.3` | GPM. Also the no-flow guard, the detector's primary false-positive filter. Draws below this are not detected — a steady 0.11–0.16 GPM draw measures as `deterministic_idle`. Measured over 10.4 days before leaving it here (#180): sub-threshold pump-off readings are 3.2× enriched within 30 s of a pump-off edge, so lowering the floor admits the pump's own collapsing loop flow, and 89 % of the rest fall within 60 s of a reading already above 0.3 — the shoulders of draws already detected. Isolated small draws run about one per ten days, and a lower floor buys 0 s of median onset lead. See `AGENTS.md` §11.4 before changing it. |
| `thermal_collapse_rate` | float | `0.05` | °F/s — tank cooling faster than this signals a draw |
| `dhw_charge_drop_rate` | float | `0.005` | %/s — charge falling faster than this signals a draw |
| `pump_on_demand_flow_threshold` | float | `0.3` | GPM of **computed** demand (`flow − pump_flow`) above which a pump-on draw is declared. Shares `flow_threshold`'s value deliberately, so both pump regimes agree on what counts as flow |
| `pump_on_demand_min_speed_rpm` | float | `1950` | RPM below which the subtraction is not trusted. The pump *estimates* its loop flow rather than metering it, and below ~2000 RPM the estimate reads low, so the difference goes spuriously positive with no draw (+0.45 GPM measured at 1650). Admits the whole production range — the pump's own 29-day minimum was 1971 RPM |
| `pump_on_demand_max_stale_seconds` | int | `30` | s — how old the `pump_flow` reading may be and still be differenced. The pump reports every 10 s |
| `flow_max_stale_seconds` | int | `60` | s — the same bound for `flow`. Deliberately looser: the meter reports on change, at a median 28 s while flowing, so matching the pump's 30 s would reject half of normal cadence |
| `dhw_in_use_min_seconds` | int | `70` | s — how long `dhw_in_use` must stay **continuously** high before it may declare a pump-on draw on its own. The flag fires ~77 times a day with a median duration of 15 s, so it is unusable bare; 70 s clears 89.7 % of its events. `0` accepts the bare flag (bench/debug only). A NaN sample breaks the run exactly as a low one does, so a BLE dropout resets the timer rather than holding the last value |
| `flow_latch_seconds` | int | `30` | s — how long flow keeps counting after it stops |
| `pump_on_demand_settle_seconds` | int | `10` | s — how long after a pump start the subtraction declines, because the pump's own loop-flow estimate is still spinning up and reads low against a loop the meter already sees moving. Measured across 296 starts: 0–10 s is p90 0.820 GPM with 26.6 % above the 0.3 threshold, against a flat 6–9 % from 10 s out to 180 s. Not covered by `pump_on_demand_min_speed_rpm`, which is for a low *steady* speed — this fires at 3172 RPM during the overshoot. `0` disables |
| `pump_on_continuation_max_seconds` | int | `300` | s — how long the continuation tier may keep asserting a draw that no measurement has *supported* for that long. A subtraction clearing `pump_on_demand_flow_threshold` re-stamps the support time, so this is not a ceiling on the pump run: a draw being measured tick after tick never expires. It bounds the blind case — chiefly a pump turning below `pump_on_demand_min_speed_rpm`, where no measurement of household draw exists to support or contradict the claim. `0` disables the tier rather than unbounding it |
| `latch_pump_off_suppression_seconds` | int | `30` | s — disarms the latch above for this long after a pump-off edge. Loop flow collapses through the flow threshold within seconds of the motor parking, so without this the latch is armed by the pump's own flow and holds a thermal/charge verdict alive on it. Defaults to the latch's own reach, so no shutdown reading can arm it. `0` disables |
| `session_gap_tolerance_seconds` | int | `60` | s — a lull shorter than this does not end a session |
| `demand_release_seconds` | int | `30` | s — how long demand stays latched after the last positive tick. Set to `0` to publish the raw per-tick result |
| `update_interval` | time | `10s` | Detection tick interval |

> The four `pump_on_demand_*` / `flow_max_stale_seconds` keys only matter when
> `pump_flow` and `motor_speed` are wired.

> Migration: `flow_max_stale_seconds` was previously spelled
> `droplet_max_stale_seconds`, after the meter one installation happened to
> use. Behavior and default are unchanged; a config still setting the old name
> fails validation naming the replacement.

> The two staleness bounds exist because a difference of two quantities is only
> meaningful if both are current, and ESPHome carries no provenance on a sensor
> value — `has_state()` says a reading exists, not how old it is. Measured over
> 30 days, gaps in the pump's flow channel beyond 20 s are 1 % of gaps but **14 %
> of pump running time**, so a last-known-value read is stale about a seventh of
> the time the pump runs.

> Unit trap: `alpha_hwr` publishes flow in m³/h while the detector expects GPM.
> Convert with a `platform: copy` sensor and a `multiply` filter
> (1 m³/h = 4.40287 GPM) — see the header comment in
> `packages/dhw_demand_detector.yaml`.

### Example

```yaml
dhw_demand:
  update_interval: 10s
  demand:
    name: "DHW Demand"
  confidence:
    name: "DHW Detection Confidence"
  demand_level:
    name: "DHW Demand Level"
  session_duration:
    name: "DHW Session Duration"
  detection_method:
    name: "DHW Detection Method"
  flow: dhw_flow
  tank_lower_temp: dhw_tank_lower_temp
  dhw_charge: dhw_charge_pct
  flow_threshold: 0.3
  demand_release_seconds: 30
```

To diagnose detection behavior, watch the component's own `confidence` and
`detection_method` sensors — `detection_method` names the exact rule that fired on
each tick, and the component logs a per-tick `VERBOSE` line with every input.

| `detection_method` | Meaning |
| --- | --- |
| `deterministic_flow` | Household flow above threshold (pump off) |
| `deterministic_thermal` | Tank temperature collapsing |
| `deterministic_charge` | Tank charge dropping |
| `deterministic_continuation` | Draw was already active when the pump started, and neither the subtraction nor `pump_on_continuation_max_seconds` has ended it |
| `deterministic_pump_on_subtraction` | Pump on, and `flow − pump_flow` measured a draw |
| `flow_onset_pending` | First tick of flow, awaiting confirmation |
| `demand_release_hold` | No live signal; demand latched by `demand_release_seconds` |
| `deterministic_dhw_in_use` | Pump on, nothing above it fired, and the heater's flag has been continuously high past `dhw_in_use_min_seconds` |
| `pump_on_uncertain` | Pump running, and nothing could separate a draw from recirculation — either the subtraction measured no draw, or one of its guards declined (a channel missing, a reading stale, or the pump below `pump_on_demand_min_speed_rpm`) |
| `deterministic_idle` | No demand, high confidence |
| `no_flow` | Flow below threshold and the latch has expired |
