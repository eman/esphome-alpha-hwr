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

Run with a Python that has aioesphomeapi available; the esphome venv works:
  venv/bin/python tools/link_gap_report.py snapshot --host hwr-pump.local \\
      --secrets secrets.yaml
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

# Evidence required before recommending anything at all.
MIN_DAYS = 14.0
MIN_NODES = 2
MAX_TRUNCATED_FRACTION = 0.01

# The component's fixed poll interval, used only to estimate how many intervals
# a span of watched time contains. update_interval is not in the component's
# schema, so this is a constant rather than a guess.
POLL_INTERVAL_S = 10.0

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

    return {
        "host": host,
        "at": time.time(),
        "over": over,
        "truncated": truncated,
        "watched_s": watched,
    }


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
        self.snapshots = 0
        self._prev: dict[str, float] = {}

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
        if reset:
            self.resets += 1

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

        self._prev = current

    @property
    def days(self) -> float:
        return self.watched_s / 86400.0

    @property
    def estimated_intervals(self) -> float:
        return self.watched_s / POLL_INTERVAL_S


def pool(records: list[dict[str, Any]]) -> dict[str, NodeTotals]:
    by_host: dict[str, NodeTotals] = {}
    for record in sorted(records, key=lambda r: float(r.get("at", 0.0))):
        host = str(record.get("host", "?"))
        by_host.setdefault(host, NodeTotals(host)).add(record)
    return by_host


# --------------------------------------------------------------------------
# The report
# --------------------------------------------------------------------------


def rate_per_day(count: float, days: float) -> float:
    return count / days if days > 0 else float("inf")


def print_coverage(nodes: dict[str, NodeTotals]) -> None:
    print("COVERAGE")
    print(f"  {'node':<24} {'watched':>10} {'snapshots':>10} {'reboots':>8} {'truncated':>10}")
    for node in nodes.values():
        print(f"  {node.host:<24} {node.days:>9.2f}d {node.snapshots:>10} {node.resets:>8} {int(node.truncated):>10}")
    total_days = sum(n.days for n in nodes.values())
    print(f"  {'pooled':<24} {total_days:>9.2f}d")


def print_survival(nodes: dict[str, NodeTotals], reported: list[int]) -> None:
    print("\nSURVIVAL  (intervals longer than T, and the rate they arrived at)")
    header = f"  {'T':>5} {'pooled':>8} {'/node-day':>10}   "
    header += "  ".join(f"{n.host:>16}" for n in nodes.values())
    print(header)
    total_days = sum(n.days for n in nodes.values())
    for threshold in reported:
        pooled = sum(n.over[threshold] for n in nodes.values())
        row = f"  {threshold:>4}s {int(pooled):>8} {rate_per_day(pooled, total_days):>10.4f}   "
        row += "  ".join(f"{int(n.over[threshold]):>16}" for n in nodes.values())
        print(row)


def print_budgets(nodes: dict[str, NodeTotals], reported: list[int]) -> list[int]:
    print("\nBUDGETS  (what each candidate default would have cost)")
    print(f"  {'T':>5} {'recycles/day':>13} {'days between':>13} {'worst node':>13}  verdict")
    passing: list[int] = []
    total_days = sum(n.days for n in nodes.values())
    for threshold in reported:
        pooled = sum(n.over[threshold] for n in nodes.values())
        rate = rate_per_day(pooled, total_days)
        worst = max((rate_per_day(n.over[threshold], n.days) for n in nodes.values()), default=rate)
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
    if len(nodes) < MIN_NODES:
        problems.append(f"{len(nodes)} installation(s) reporting, need {MIN_NODES} independent ones")

    truncated = sum(n.truncated for n in nodes.values())
    intervals = sum(n.estimated_intervals for n in nodes.values())
    fraction = truncated / intervals if intervals > 0 else 1.0
    if fraction > MAX_TRUNCATED_FRACTION:
        problems.append(
            f"{fraction * 100:.2f}% of intervals were cut short rather than ending on their own "
            f"-- the tail was truncated, not observed"
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
    print("  - A counter at or above the data_timeout in force cannot fill. Check --budget.")
    print("  - Counts between a node's last snapshot and a reboot are lost. Frequent")
    print("    snapshots keep that tail small; the reboot count above says how often it")
    print("    happened.")


def cmd_report(log_path: str, budget_s: float | None) -> int:
    records: list[dict[str, Any]] = []
    try:
        with open(log_path) as handle:
            records = [json.loads(line) for line in handle if line.strip()]
    except OSError as exc:
        die(f"could not read {log_path!r}: {exc}")
    if not records:
        die(f"{log_path!r} holds no snapshots")

    nodes = pool(records)
    reported = sorted({t for node in nodes.values() for t in node.reported})
    if not reported:
        die("no gap histogram entities in any snapshot -- are they declared?")

    print_coverage(nodes)
    print_survival(nodes, reported)
    passing = print_budgets(nodes, reported)
    print_recommendation(passing, reported, refusals(nodes, reported, budget_s))
    print_caveats()
    return 0


# --------------------------------------------------------------------------


def parse_targets(argv: list[str]) -> list[tuple[str, str]]:
    """Collect (host, key) pairs. --key/--secrets apply to every later --host."""
    key = os.getenv("ALPHA_HWR_API_KEY", "")
    targets: list[tuple[str, str]] = []
    items = iter(argv)
    for arg in items:
        if arg == "--host":
            host = next(items, "")
            if not host:
                die("--host needs a value")
            if not key:
                die(f"no api key for {host}: pass --key or --secrets before it")
            targets.append((host, key))
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
    if not targets:
        host = os.getenv("ALPHA_HWR_HOST", "")
        if host and key:
            targets.append((host, key))
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
