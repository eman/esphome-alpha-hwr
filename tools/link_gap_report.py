#!/usr/bin/env python3
"""
Turn the link gap histogram into a `data_timeout` default (issue #176 part 1).

The firmware counts, for each of 15/20/30/45/60/90 s, how many quiet intervals
ran longer than that -- which is exactly how many times a `data_timeout` of that
length would have fired -- plus how many intervals were cut short rather than
ending on their own, and how much live-link time the counts were drawn from.
This reads those counters off one or more nodes and answers the question the
issue asks: what should the default be, and is there enough evidence to say?

Usage:
  link_gap_report.py snapshot [--log FILE] --host H --secrets secrets.yaml [--host H2 ...]
  link_gap_report.py report   [--log FILE] [--budget 600]

`snapshot` reads the counters and appends one record per node to the log.
`report` reconstructs each node's totals from the log and prints the analysis.

Why a log rather than a single live read: the counters are RAM values that
restart at every boot, so one read covers only the current boot, and a
measurement run of several weeks will meet an OTA. Snapshots taken periodically
(weekly is plenty) are pooled here with the same reset detection Home
Assistant's `total_increasing` statistics use -- a reading lower than the one
before it starts a new window instead of being subtracted. Home Assistant's
long-term statistics graphs hold the same information and are a fine
cross-check; this exists so the analysis does not depend on having them.

Connection settings match tools/write_bench.py (flag overrides environment):
  --host / ALPHA_HWR_HOST          node address (e.g. hwr-pump.local)
  --key  / ALPHA_HWR_API_KEY       api encryption key (base64)
  --secrets <file>                 read the key from a secrets.yaml (api_key:)

Repeat --host to read several pumps in one run; --key/--secrets before a --host
applies to it and to every later one until overridden. One connection per node,
opened and closed: connection count is what costs heap on these nodes.

`snapshot` needs aioesphomeapi; the esphome venv provides it. `report` needs
nothing but the log file, so it runs on a plain python3.

  venv/bin/python tools/link_gap_report.py snapshot --host hwr-pump.local \\
      --secrets secrets.yaml
  python3 tools/link_gap_report.py report --budget 600

The procedure around these -- raising data_timeout before declaring the
entities, the snapshot cadence and why it matters, and how to read each section
of the output -- is in docs/configuration.md under "Running a measurement run".
"""

from __future__ import annotations

import argparse
import asyncio
import json
import math
import os
import re
import sys
import time
from typing import Any

# Rungs the firmware counts against, in seconds. Mirrors LINK_GAP_THRESHOLDS_MS
# in components/alpha_hwr/link_watchdog.h; a rung missing from a node's config
# simply goes unreported rather than being assumed zero.
THRESHOLDS_S = [15, 20, 30, 45, 60, 90]

# The smallest budget that is defensible on the firmware's own timings: 31.5 s
# is the calculated worst case from connection-open to first inbound data with
# the sequence backstop firing (link_watchdog.h), and 30% margin on a worst case
# derived from constants rather than measured is not generous.
FLOOR_S = 41

# What a spurious recycle is worth. Each one takes another run at the
# encryption-on-open window that can erase the bond (issue #14), so the budget
# is deliberately stingy: less than one per installation per 30 days.
TOLERANCE_PER_DAY = 1.0 / 30.0

# Evidence required before recommending anything at all. Deliberately only
# properties of the DATA -- how long it ran, whether the tail was clipped,
# whether the budget let the rungs fill. There is no minimum node count: how
# many installations exist is a fact about the world, not about the evidence,
# and gating on it made the report refuse forever on a one-pump setup no matter
# how clean the month it had. What a single installation cannot support is the
# claim that the number GENERALISES, and that belongs in the caveats with the
# other limits.
MIN_DAYS = 14.0
# Truncated intervals are observations whose true length is unknown, so a run
# full of them has a tail that was clipped rather than measured. Expressed per
# watched day rather than as a fraction of intervals: the number of intervals is
# NOT watched_s / poll_interval, because one 10 s poll cycle queues five
# telemetry reads plus the schedule read and every reply closes an interval of
# its own -- an estimate built on the poll interval undercounts them severalfold
# and overstates the truncated share by the same factor. The firmware does not
# publish an interval count, so this uses a denominator that is exactly known.
MAX_TRUNCATED_PER_DAY = 2.0

GAP_RE = re.compile(r"gaps?\s+over\s+(\d+)\s*s", re.IGNORECASE)
TRUNCATED_RE = re.compile(r"gaps?\s+truncated", re.IGNORECASE)
WATCH_RE = re.compile(r"watched\s+time", re.IGNORECASE)


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(2)


# --------------------------------------------------------------------------
# Reading a node
# --------------------------------------------------------------------------


async def read_node(host: str, key: str) -> dict[str, Any]:
    """One node's current counters, as a snapshot record."""
    # Imported here rather than at module scope so `report` runs on a plain
    # python3: the analysis needs nothing but the log file, and requiring
    # aioesphomeapi to read a JSON file would be a dependency on the wrong half.
    from write_bench import connect, maybe_await

    client = await connect(host, key)
    try:
        entities, _ = await client.list_entities_services()
        names = {e.key: getattr(e, "name", "") for e in entities}
        seen: dict[int, Any] = {}
        done = asyncio.Event()

        def on_state(state: Any) -> None:
            seen[state.key] = state
            if len(seen) >= len(names):
                done.set()

        await maybe_await(client.subscribe_states(on_state))
        try:
            await asyncio.wait_for(done.wait(), 10)
        except asyncio.TimeoutError:
            pass
    finally:
        await maybe_await(client.disconnect())

    over: dict[str, float] = {}
    truncated: float | None = None
    watched: float | None = None
    for entity_key, state in seen.items():
        name = names.get(entity_key, "")
        value = getattr(state, "state", None)
        if not isinstance(value, float) or math.isnan(value):
            continue
        match = GAP_RE.search(name)
        if match:
            over[match.group(1)] = value
        elif TRUNCATED_RE.search(name):
            truncated = value
        elif WATCH_RE.search(name):
            watched = value

    record: dict[str, Any] = {
        "host": host,
        "at": time.time(),
        "over": over,
        "truncated": truncated,
        "watched_s": watched,
    }
    if watched is None:
        # Only when discovery found nothing, and only to make that diagnosable:
        # the caller prints these so a renamed entity is distinguishable from a
        # missing one. Not kept otherwise -- the log is meant to stay small and
        # to carry measurements, not an inventory.
        record["seen_names"] = sorted(n for n in names.values() if n and "link" in n.lower())
    return record


async def cmd_snapshot(targets: list[tuple[str, str]], log_path: str) -> int:
    records = []
    for host, key in targets:
        try:
            records.append(await read_node(host, key))
        except Exception as exc:  # noqa: BLE001 - one unreachable node is not fatal
            print(f"WARN: {host}: {exc}", file=sys.stderr)
    if not records:
        die("no node could be read")

    with open(log_path, "a") as handle:
        for record in records:
            handle.write(json.dumps(record) + "\n")

    for record in records:
        if record["watched_s"] is None:
            print(f"{record['host']}: no gap histogram entities found")
            # Discovery matches the documented display names, because nothing the
            # API exposes ties an entity back to the config key that made it --
            # object_id is derived from the name too. So a node whose entities
            # were renamed looks identical to one that never declared them. Show
            # what was actually there rather than leaving that undiagnosable.
            seen_names = record.get("seen_names") or []
            if seen_names:
                print(f"  Sensors seen: {', '.join(seen_names[:12])}")
                print("  Discovery matches the documented names ('Pump Link Gaps Over 15s',")
                print("  'Pump Link Gaps Truncated', 'Pump Link Watched Time'). Rename to match,")
                print("  or declare the keys -- see docs/configuration.md")
            else:
                print("  Declare them in the alpha_hwr block -- see docs/configuration.md")
            continue
        days = record["watched_s"] / 86400.0
        rungs = " ".join(f">{t}s={int(record['over'][t])}" for t in sorted(record["over"], key=int))
        print(f"{record['host']}: watched {days:.2f}d  {rungs}  truncated={int(record['truncated'] or 0)}")
    print(f"\nAppended {len(records)} record(s) to {log_path}")
    return 0


# --------------------------------------------------------------------------
# Pooling
# --------------------------------------------------------------------------


class NodeTotals:
    """One node's counters accumulated across boots.

    Each field is a sum over disjoint observation windows. A reading lower than
    the previous one means the node rebooted and the counters restarted, so the
    new reading is a fresh window rather than a difference -- the same rule Home
    Assistant applies to a `total_increasing` sensor. Anything the node counted
    between the last snapshot and the reboot is lost, which is why snapshots
    should be frequent enough that a lost tail does not matter.
    """

    def __init__(self, host: str) -> None:
        self.host = host
        self.over: dict[int, float] = {t: 0.0 for t in THRESHOLDS_S}
        self.reported: set[int] = set()
        self.truncated = 0.0
        self.watched_s = 0.0
        self.resets = 0
        self.ambiguous = 0
        self.snapshots = 0
        self._prev: dict[str, float] = {}
        self._prev_at: float | None = None

    def add(self, record: dict[str, Any]) -> None:
        if record.get("watched_s") is None:
            return
        self.snapshots += 1
        current: dict[str, float] = {"watched_s": float(record["watched_s"])}
        current["truncated"] = float(record.get("truncated") or 0.0)
        for key, value in (record.get("over") or {}).items():
            current[f"over{int(key)}"] = float(value)
            self.reported.add(int(key))
            # A node may report a rung this tool has not heard of, if the
            # firmware's ladder has moved on. Carry it rather than raising:
            # an unknown rung is still a usable data point, and a report that
            # crashes on the newer node is worse than one that shows it.
            self.over.setdefault(int(key), 0.0)

        # One reset test for the whole record, on watched time: it is the field
        # that always moves on a live link, so a counter that merely happens not
        # to have changed cannot be mistaken for a reboot.
        reset = bool(self._prev) and current["watched_s"] < self._prev.get("watched_s", 0.0)
        at = float(record.get("at", 0.0))
        if reset:
            self.resets += 1
        elif self._prev and self._prev_at is not None:
            # A reboot is only visible here when the new reading is LOWER. One
            # that happened early enough to climb back past the previous total
            # before this snapshot is invisible, and differencing then drops the
            # whole pre-reboot session.
            #
            # That is exactly detectable, though not correctable: hiding needs
            # the post-reboot watched time to exceed the previous total, and it
            # cannot exceed the wall time since the last snapshot. So when
            # `elapsed <= previous watched`, a hidden reboot is impossible and
            # this difference is sound; otherwise it might not be, and the run
            # is only as trustworthy as the snapshot cadence. Nothing in the API
            # carries a boot identity (DeviceInfo has no uptime), so the honest
            # move is to count these and say so rather than assume.
            if at - self._prev_at > self._prev.get("watched_s", 0.0):
                self.ambiguous += 1

        for field, value in current.items():
            previous = 0.0 if reset else self._prev.get(field, 0.0)
            delta = value - previous
            if delta < 0:
                delta = value  # a counter that reset on its own
            if field == "watched_s":
                self.watched_s += delta
            elif field == "truncated":
                self.truncated += delta
            else:
                self.over[int(field[4:])] += delta

        # Merge rather than replace. read_node() writes a snapshot after a
        # timeout even when only some states arrived, and replacing wholesale
        # would forget the baselines of the fields that snapshot omitted -- so
        # the next one to carry them would add their entire since-boot value a
        # second time. After a reset, 0 IS the right baseline for a field that
        # did not appear, which is what a plain replace gives.
        self._prev = dict(current) if reset else {**self._prev, **current}
        self._prev_at = at

    @property
    def days(self) -> float:
        return self.watched_s / 86400.0


def pool(records: list[dict[str, Any]]) -> tuple[dict[str, NodeTotals], list[str]]:
    """Per-node totals, plus the hosts that reported no histogram at all.

    A node whose snapshots carry no histogram entities must not become a
    NodeTotals: it would appear with 0 watched days, satisfy the "need two
    installations" refusal, and — because a rate over 0 days is infinite —
    make every candidate budget FAIL, so the report would print "no candidate
    clears both" as if that were a finding about the pump rather than about a
    node that was never instrumented.
    """
    by_host: dict[str, NodeTotals] = {}
    silent: list[str] = []
    for record in sorted(records, key=lambda r: float(r.get("at", 0.0))):
        host = str(record.get("host", "?"))
        if record.get("watched_s") is None:
            if host not in silent:
                silent.append(host)
            continue
        by_host.setdefault(host, NodeTotals(host)).add(record)
    return by_host, [h for h in silent if h not in by_host]


# --------------------------------------------------------------------------
# The report
# --------------------------------------------------------------------------


def rate_per_day(count: float, days: float) -> float:
    return count / days if days > 0 else float("inf")


def print_coverage(nodes: dict[str, NodeTotals]) -> None:
    print("COVERAGE")
    print(f"  {'node':<24} {'watched':>10} {'snapshots':>10} {'reboots':>8} {'unsure':>7} {'truncated':>10}")
    for node in nodes.values():
        print(
            f"  {node.host:<24} {node.days:>9.2f}d {node.snapshots:>10} "
            f"{node.resets:>8} {node.ambiguous:>7} {int(node.truncated):>10}"
        )
    total_days = sum(n.days for n in nodes.values())
    print(f"  {'pooled':<24} {total_days:>9.2f}d")


def print_survival(nodes: dict[str, NodeTotals], reported: list[int]) -> None:
    print("\nSURVIVAL  (intervals longer than T, and the rate they arrived at)")
    header = f"  {'T':>5} {'pooled':>8} {'/node-day':>10}   "
    header += "  ".join(f"{n.host:>16}" for n in nodes.values())
    print(header)
    total_days = sum(n.days for n in nodes.values())
    for threshold in reported:
        pooled = sum(n.over.get(threshold, 0.0) for n in nodes.values())
        row = f"  {threshold:>4}s {int(pooled):>8} {rate_per_day(pooled, total_days):>10.4f}   "
        row += "  ".join(f"{int(n.over.get(threshold, 0.0)):>16}" for n in nodes.values())
        print(row)


def print_budgets(nodes: dict[str, NodeTotals], reported: list[int]) -> list[int]:
    print("\nBUDGETS  (what each candidate default would have cost)")
    print(f"  {'T':>5} {'recycles/day':>13} {'days between':>13} {'worst node':>13}  verdict")
    passing: list[int] = []
    total_days = sum(n.days for n in nodes.values())
    for threshold in reported:
        pooled = sum(n.over.get(threshold, 0.0) for n in nodes.values())
        rate = rate_per_day(pooled, total_days)
        # Only nodes that actually observed something can bound the worst case.
        # A zero-day node yields an infinite rate, which would fail every rung.
        per_node = [rate_per_day(n.over.get(threshold, 0.0), n.days) for n in nodes.values() if n.days > 0]
        worst = max(per_node, default=rate)
        between = 1.0 / rate if rate > 0 else float("inf")
        ok = threshold >= FLOOR_S and worst < TOLERANCE_PER_DAY
        if ok:
            passing.append(threshold)
        why = "PASS"
        if threshold < FLOOR_S:
            why = f"FAIL below the {FLOOR_S}s floor"
        elif worst >= TOLERANCE_PER_DAY:
            why = "FAIL over the recycle tolerance"
        between_text = "never" if math.isinf(between) else f"{between:>12.1f}"
        print(f"  {threshold:>4}s {rate:>13.4f} {between_text:>13} {worst:>13.4f}  {why}")
    return passing


def refusals(nodes: dict[str, NodeTotals], reported: list[int], budget_s: float | None) -> list[str]:
    problems: list[str] = []
    total_days = sum(n.days for n in nodes.values())
    if total_days < MIN_DAYS:
        problems.append(
            f"only {total_days:.1f} days of watched link, and the issue asks for weeks (need {MIN_DAYS:.0f})"
        )
    truncated = sum(n.truncated for n in nodes.values())
    truncated_rate = rate_per_day(truncated, total_days) if total_days > 0 else 0.0
    if truncated_rate > MAX_TRUNCATED_PER_DAY:
        problems.append(
            f"{truncated:.0f} intervals were cut short rather than ending on their own "
            f"({truncated_rate:.2f} per watched day) -- the tail was clipped, not observed"
        )

    if budget_s is None:
        problems.append("no --budget given, so it cannot be checked that the rungs could fill")
    else:
        blind = [t for t in reported if t >= budget_s]
        if blind:
            problems.append(
                f"data_timeout was {budget_s:.0f}s during the run, so the "
                f"{', '.join(f'{t}s' for t in blind)} rung(s) could not fill and read zero "
                f"whatever the pump did"
            )
    return problems


def print_recommendation(passing: list[int], reported: list[int], problems: list[str]) -> None:
    print("\nRECOMMENDATION")
    if problems:
        print("  INSUFFICIENT EVIDENCE -- keep the current default. Because:")
        for problem in problems:
            print(f"    - {problem}")
    elif not passing:
        print("  No candidate clears both the floor and the recycle tolerance.")
        print("  Either the link is genuinely this variable, or the run caught a fault;")
        print("  check link_recycles and the Pump Link Fault history before widening.")
    else:
        chosen = passing[0]
        alternative = next((t for t in reported if t > chosen), None)
        print(f"  data_timeout: {chosen}s")
        if alternative is not None:
            print(f"  Conservative alternative: {alternative}s")

    print("\n  Decision rule")
    print(f"    Floor      T >= {FLOOR_S}s. 31.5s is the calculated worst case from")
    print("               connection-open to first inbound data with the sequence")
    print("               backstop firing (link_watchdog.h), plus 30% margin on a")
    print("               number derived from constants rather than measured.")
    print(f"    Tolerance  Under {TOLERANCE_PER_DAY:.4f} spurious recycles per node-day (one per")
    print("               30 days). Each recycle takes another run at the")
    print("               encryption-on-open window that can erase the bond (#14).")
    print("    Choose     The smallest rung meeting both. Detection latency is the")
    print("               cost on the other side and these counters cannot measure")
    print("               it -- that half is judgement, said here so it is not")
    print("               mistaken for arithmetic.")


def print_caveats() -> None:
    print("\nCAVEATS")
    print("  - 'recycles/day' is an UPPER bound on what a budget of T would cost. Inside a")
    print("    single deaf episode the backoff widens the window after the first recycle, so")
    print("    repeated excursions do not each cost one. It under-states the benefit for the")
    print("    same reason: a long deaf episode appears here as one interval. The rule")
    print("    budgets the cost, which is the side that argues against lowering the default.")
    print("  - Rates are per day of WATCHED link, not per calendar day. Time disconnected is")
    print("    not sampled, because the watchdog is not running then either.")
    print("  - A recommendation describes the installations that reported it. One pump")
    print("    characterises that pump; whether the number carries to a different site,")
    print("    radio environment or pump firmware is not something these counters can")
    print("    answer. Add nodes to the log as they appear and rerun -- they pool.")
    print("  - A counter at or above the data_timeout in force cannot fill. Check --budget.")
    print("  - Counts between a node's last snapshot and a reboot are lost. Frequent")
    print("    snapshots keep that tail small; the reboot count above says how often it")
    print("    happened.")
    print("  - The 'unsure' column counts snapshot intervals long enough that a reboot")
    print("    could have happened and climbed back past the previous total unseen, which")
    print("    would silently drop that session. Snapshot more often than the node's")
    print("    watched time grows and the column goes to zero; a nonzero count means the")
    print("    totals are a lower bound.")


def cmd_report(log_path: str, budget_s: float | None) -> int:
    records: list[dict[str, Any]] = []
    try:
        with open(log_path) as handle:
            records = [json.loads(line) for line in handle if line.strip()]
    except OSError as exc:
        die(f"could not read {log_path!r}: {exc}")
    if not records:
        die(f"{log_path!r} holds no snapshots")

    nodes, silent = pool(records)
    reported = sorted({t for node in nodes.values() for t in node.reported})
    if not reported:
        die("no gap histogram entities in any snapshot -- are they declared?")

    print_coverage(nodes)
    if silent:
        print(f"  not instrumented, excluded: {', '.join(silent)}")
    print_survival(nodes, reported)
    passing = print_budgets(nodes, reported)
    problems = refusals(nodes, reported, budget_s)
    if silent:
        problems.append(
            f"{', '.join(silent)} reported no histogram entities and were excluded -- "
            f"the fleet is smaller than it looks"
        )
    # A ladder one node reports and another does not is counted as zero for the
    # node that is missing it, which understates. Say so rather than silently
    # pooling the two.
    partial = [t for t in reported if any(t not in n.reported for n in nodes.values())]
    if partial:
        problems.append(
            f"not every node reports the {', '.join(f'{t}s' for t in partial)} rung(s), "
            f"so the pooled counts for those are understated"
        )
    print_recommendation(passing, reported, problems)
    print_caveats()
    return 0


# --------------------------------------------------------------------------


def parse_targets(argv: list[str]) -> list[tuple[str, str]]:
    """Collect (host, key) pairs.

    A --key/--secrets binds to every --host after it, so several pumps with
    different keys can be read in one run. A host given before any key is not an
    error: it takes whichever key turns up later, which is what makes the
    obvious `--host H --secrets file` ordering work. Only a host with no key
    anywhere is refused.
    """
    key = os.getenv("ALPHA_HWR_API_KEY", "")
    pending: list[tuple[str, str]] = []
    items = iter(argv)
    for arg in items:
        if arg == "--host":
            host = next(items, "")
            if not host:
                die("--host needs a value")
            pending.append((host, key))
        elif arg == "--key":
            key = next(items, "")
        elif arg == "--secrets":
            path = next(items, "")
            try:
                import yaml

                with open(path) as handle:
                    key = yaml.safe_load(handle)["api_key"]
            except Exception as exc:  # noqa: BLE001 - report and exit
                die(f"could not read api_key from {path!r}: {exc}")
    if not pending:
        host = os.getenv("ALPHA_HWR_HOST", "")
        if host:
            pending.append((host, key))

    # A host that had no key when it was parsed takes the last one seen, so
    # order does not matter for the common single-pump case.
    targets = [(host, host_key or key) for host, host_key in pending]
    for host, host_key in targets:
        if not host_key:
            die(f"no api key for {host}: pass --key, --secrets <file>, or set ALPHA_HWR_API_KEY")
    return targets


def main() -> int:
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("command", choices=["snapshot", "report"])
    parser.add_argument("--log", default="link_gap_log.jsonl")
    parser.add_argument("--budget", type=float, default=None)
    args, rest = parser.parse_known_args()

    if args.command == "snapshot":
        targets = parse_targets(rest)
        if not targets:
            die("no node to read: pass --host (and --key or --secrets)")
        return asyncio.run(cmd_snapshot(targets, args.log))
    return cmd_report(args.log, args.budget)


if __name__ == "__main__":
    sys.exit(main())
