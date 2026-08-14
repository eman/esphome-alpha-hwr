#!/usr/bin/env python3
"""
Bench client for the alpha_hwr programmatic write interface (issue #92).

Talks to the node over the ESPHome native API: lists the registered services,
snapshots entity states, and calls a write service then waits for the matching
esphome.alpha_hwr_write_settled event. Prints every settle event it sees and
verifies the one-terminal-event-per-operation contract (flags a missing or
duplicated terminal event as a violation).

Usage:
  write_bench.py services
  write_bench.py states
  write_bench.py call <service> [k=v ...] [--timeout N] [--linger N]
  write_bench.py chain <service> [k=v ...] -- <service> [k=v ...] -- ...
  write_bench.py burst <service> <common k=v ...> --each k=v[,k=v] [--each ...]
  write_bench.py upload <enabled 0|1|-> [layer,day,sh,sm,eh,em ...]

`call` auto-generates an op_id if none is passed. `burst` fires one call per
--each group (merged over the common args) back to back on a single
connection, which is how to exercise the queued-write supersede path.

`chain` runs several DIFFERENT services over one connection, resolving every
service once up front. Prefer it to invoking `call` repeatedly.

The reason is heap, but not the reason it first appears. A bench OOM on
2026-08-13 aborted inside encode_list_service_response() with four clients
stacked on top of Home Assistant, which looks like the service-list encode
being the load. It is not: ListEntitiesServicesArgument is 12 bytes and no
service here takes more than 4 arguments, so that allocation is at most ~48
bytes and is freed as soon as the message is sent. A 48-byte allocation
failing means the heap was already exhausted -- the encode was the victim,
not the cause. The real per-client cost is the APIConnection and its frame
buffers, so what matters is the number of CONNECTIONS, which is what this
command reduces. Resolving services per call would not help, since
list_entities_services() is uncached and re-lists every time.

Connection settings (flag overrides environment):
  --host / ALPHA_HWR_HOST          node address (e.g. hwr-pump.local)
  --key  / ALPHA_HWR_API_KEY       api encryption key (base64)
  --secrets <file>                 read the key from a secrets.yaml (api_key:)

Run with a Python that has aioesphomeapi available; the esphome venv works:
  venv/bin/python tools/write_bench.py --host hwr-pump.local --secrets secrets.yaml services

Examples:
  write_bench.py call pump_set_setpoint mode=constant_speed value=2000
  write_bench.py call set_schedule_entry data=3,1,6,0,8,0
  write_bench.py upload 1 0,0,6,54,7,0 0,1,7,24,7,30 1,0,17,54,18,0
  write_bench.py burst pump_set_setpoint mode=constant_speed \\
      --each value=1800 --each value=1900 --each value=2100
"""

from __future__ import annotations

import asyncio
import inspect
import json
import os
import sys
import time
from collections.abc import Iterable
from typing import Any

from aioesphomeapi import APIClient

EVENT = "esphome.alpha_hwr_write_settled"


def die(msg: str) -> None:
    print(f"ERROR: {msg}", file=sys.stderr)
    sys.exit(2)


def resolve_connection(args: list[str]) -> tuple[str, str, list[str]]:
    """Extract --host/--key/--secrets from args; fall back to environment."""
    host = os.getenv("ALPHA_HWR_HOST", "")
    key = os.getenv("ALPHA_HWR_API_KEY", "")
    rest: list[str] = []
    it = iter(args)
    for a in it:
        if a == "--host":
            host = next(it, "")
        elif a == "--key":
            key = next(it, "")
        elif a == "--secrets":
            path = next(it, "")
            try:
                import yaml

                with open(path) as f:
                    key = yaml.safe_load(f)["api_key"]
            except Exception as e:  # noqa: BLE001 - report and exit
                die(f"could not read api_key from {path!r}: {e}")
        else:
            rest.append(a)
    if not host:
        die("no host: pass --host or set ALPHA_HWR_HOST")
    if not key:
        die("no api key: pass --key, --secrets <file>, or set ALPHA_HWR_API_KEY")
    return host, key, rest


async def maybe_await(x: Any) -> Any:
    if inspect.isawaitable(x):
        return await x
    return x


async def connect(host: str, key: str) -> APIClient:
    client = APIClient(host, 6053, None, noise_psk=key)
    await client.connect(login=True)
    return client


def kv_dict(items: Iterable[str]) -> dict[str, str]:
    """Parse `k=v` strings into a dict.

    `str.split("=", 1)` yields a list, which dict()/dict.update() accept at
    runtime but which is not a `tuple[str, str]` as far as a type checker is
    concerned. partition() gives a real tuple and also handles a bare `k`
    (empty value) without raising.
    """
    out: dict[str, str] = {}
    for item in items:
        if not item:
            continue
        k, _, v = item.partition("=")
        out[k] = v
    return out


def coerce(service: Any, data: dict[str, str]) -> dict[str, Any]:
    """Coerce k=v strings to the service's declared argument types."""
    out: dict[str, Any] = {}
    types = {a.name: getattr(a.type, "name", str(a.type)) for a in service.args}
    for k, v in data.items():
        t = types.get(k, "STRING")
        if "BOOL" in t:
            out[k] = v.lower() in ("1", "true", "on", "yes")
        elif "FLOAT" in t:
            out[k] = float(v)
        elif "INT" in t:
            out[k] = int(v)
        else:
            out[k] = v
    for a in service.args:  # default omitted args so the call is well-formed
        if a.name not in out:
            t = getattr(a.type, "name", str(a.type))
            out[a.name] = False if "BOOL" in t else 0.0 if "FLOAT" in t else 0 if "INT" in t else ""
    return out


async def find_service(client: APIClient, name: str) -> Any:
    _entities, services = await client.list_entities_services()
    service = next((s for s in services if s.name == name), None)
    if service is None:
        available = ", ".join(sorted(s.name for s in services))
        die(f"no service named {name!r}; available: {available}")
    return service


class EventCollector:
    """Subscribes to service calls and records every write_settled event."""

    def __init__(self) -> None:
        self.events: list[dict[str, str]] = []
        self.waiters: dict[str, asyncio.Event] = {}

    def expect(self, op_id: str) -> asyncio.Event:
        # Refuse a reused op_id outright. setdefault() silently handed back an
        # ALREADY-SET event, so a second call waiting on it returned instantly
        # on the first call's event and its own terminal event went unchecked --
        # a false pass in the one tool whose job is catching missing and
        # duplicate terminal events.
        if op_id in self.waiters:
            die(f"op_id {op_id!r} used twice in one run; give each call its own")
        self.waiters[op_id] = asyncio.Event()
        return self.waiters[op_id]

    def on_service_call(self, call: Any) -> None:
        if getattr(call, "service", "") != EVENT:
            return
        evt = dict(call.data)
        self.events.append(evt)
        print(f"  [{time.strftime('%H:%M:%S')}] event: {json.dumps(evt, sort_keys=True)}")
        waiter = self.waiters.get(evt.get("op_id", ""))
        if waiter is not None:
            waiter.set()

    def count(self, op_id: str) -> int:
        return sum(1 for e in self.events if e.get("op_id") == op_id)

    def report(self, op_ids: list[str]) -> int:
        """Print per-op event counts; return non-zero if the contract broke."""
        bad = 0
        for op_id in op_ids:
            n = self.count(op_id)
            statuses = [e.get("status") for e in self.events if e.get("op_id") == op_id]
            note = "(exactly one, contract holds)" if n == 1 else "<-- CONTRACT VIOLATION"
            bad += n != 1
            print(f"  {op_id}: {n} event(s) {statuses} {note}")
        return bad


async def cmd_services(host: str, key: str) -> int:
    client = await connect(host, key)
    _entities, services = await client.list_entities_services()
    for s in sorted(services, key=lambda s: s.name):
        args = ", ".join(f"{a.name}:{getattr(a.type, 'name', a.type)}" for a in s.args)
        print(f"{s.name}({args})")
    await maybe_await(client.disconnect())
    return 0


async def cmd_states(host: str, key: str) -> int:
    client = await connect(host, key)
    entities, _ = await client.list_entities_services()
    names = {e.key: (type(e).__name__, e.name) for e in entities}
    seen: dict[int, Any] = {}
    done = asyncio.Event()

    def on_state(state: Any) -> None:
        seen[state.key] = state
        if len(seen) >= len(names):
            done.set()

    await maybe_await(client.subscribe_states(on_state))
    try:
        await asyncio.wait_for(done.wait(), 5)
    except asyncio.TimeoutError:
        pass
    interesting = ("Select", "Number", "Switch", "TextSensor", "BinarySensor")
    for entity_key, state in sorted(seen.items(), key=lambda kv: names.get(kv[0], ("", ""))[1]):
        cls, name = names.get(entity_key, ("?", f"key={entity_key}"))
        if not any(cls.startswith(i) for i in interesting):
            continue
        val = getattr(state, "state", None)
        if isinstance(val, float):
            val = round(val, 4)
        print(f"[{cls}] {name} = {val!r}")
    await maybe_await(client.disconnect())
    return 0


async def run_call(client: APIClient, collector: EventCollector,
                   service: Any, service_name: str, kvs: list[str],
                   timeout: float, linger: float, seq: int = 0) -> int:
    """One service call on an ALREADY-CONNECTED client. Returns 0 on success.

    Split out of cmd_call so `chain` can run several calls over a single
    connection. That matters because each connection costs an APIConnection and
    its frame buffers on a node with ~72 KB free, and four stacked clients were
    enough to exhaust it (issue #127). See the module docstring for why the
    service-list encode in that backtrace was the victim rather than the cause.
    """
    # `service` is resolved by the caller. Resolving it here would defeat the
    # whole point of a shared connection: find_service() calls
    # list_entities_services(), which aioesphomeapi does NOT cache -- it sends
    # a fresh ListEntitiesRequest every time. A four-call chain that resolved
    # per call made the node encode its full service list four times, exactly
    # as four separate `call` invocations would.
    data = kv_dict(kvs)
    # Unique per call, not per second. `int(time.time())` has one-second
    # resolution and expect() uses setdefault, so two calls settling inside the
    # same second on a shared collector reused an already-set Event: the second
    # call's wait() returned instantly on the FIRST call's event and its
    # terminal event was never checked. A tool whose only job is detecting
    # missing and duplicate terminal events reported "contract holds" for a
    # call it had not verified.
    op_id = data.setdefault("op_id", f"bench-{time.monotonic_ns():x}-{seq}")
    payload = coerce(service, data)

    # The collector is owned by the caller and subscribed ONCE per connection.
    # Subscribing per call looked harmless but is not: on a shared connection
    # every earlier collector stays attached, so each of them also receives and
    # prints later events. A two-call chain printed its second settle twice.
    matched = collector.expect(op_id)

    t0 = time.monotonic()
    print(f"-> {service_name}({json.dumps(payload, sort_keys=True)})")
    await maybe_await(client.execute_service(service, payload))

    try:
        await asyncio.wait_for(matched.wait(), timeout)
        print(f"settled in {time.monotonic() - t0:.1f}s")
    except asyncio.TimeoutError:
        print(f"NO TERMINAL EVENT for op_id={op_id} within {timeout}s <-- CONTRACT VIOLATION")
        return 1

    await asyncio.sleep(linger)  # catch duplicate terminal events
    bad = collector.report([op_id])
    return 1 if bad else 0


async def cmd_call(host: str, key: str, service_name: str, kvs: list[str],
                   timeout: float, linger: float) -> int:
    client = await connect(host, key)
    try:
        collector = EventCollector()
        await maybe_await(client.subscribe_service_calls(collector.on_service_call))
        await asyncio.sleep(0.3)
        service = await find_service(client, service_name)
        return await run_call(client, collector, service, service_name,
                              kvs, timeout, linger)
    finally:
        await maybe_await(client.disconnect())


async def cmd_chain(host: str, key: str, calls: list[list[str]],
                    timeout: float, linger: float) -> int:
    """Several service calls over ONE connection.

    Use this instead of invoking `call` N times: N invocations means N
    connects, and every connect re-encodes the whole service list. See
    run_call() for why that matters on this node.
    """
    client = await connect(host, key)
    worst = 0
    try:
        collector = EventCollector()
        await maybe_await(client.subscribe_service_calls(collector.on_service_call))
        await asyncio.sleep(0.3)

        # ONE ListEntitiesRequest for the whole chain. find_service() sends
        # its own, and it is uncached, so calling it per service (let alone per
        # call) would put the node back to re-encoding its service list N
        # times -- the exact thing this command exists to avoid.
        _entities, services = await client.list_entities_services()
        by_name = {s.name: s for s in services}
        missing = sorted({c[0] for c in calls} - set(by_name))
        if missing:
            die(f"no service(s) named {', '.join(missing)}; "
                f"available: {', '.join(sorted(by_name))}")
        resolved = {name: by_name[name] for name in {c[0] for c in calls}}

        op_ids: list[str] = []
        for i, call in enumerate(calls, 1):
            print(f"\n[{i}/{len(calls)}] {call[0]}")
            before = set(collector.waiters)
            rc = await run_call(client, collector, resolved[call[0]], call[0],
                                call[1:], timeout, linger, seq=i)
            op_ids.extend(sorted(set(collector.waiters) - before))
            worst = max(worst, rc)

        # Re-check every op at the end, as `burst` does. Without this a
        # duplicate terminal event for call 1 arriving during call 3 is printed
        # but never flagged, and the exit code stays 0.
        if len(op_ids) > 1:
            print("\nfinal check across the whole chain:")
            if collector.report(op_ids):
                worst = max(worst, 1)
    finally:
        await maybe_await(client.disconnect())
    return worst


async def cmd_burst(host: str, key: str, service_name: str, common: list[str],
                    groups: list[str], timeout: float, linger: float) -> int:
    client = await connect(host, key)
    service = await find_service(client, service_name)

    collector = EventCollector()
    await maybe_await(client.subscribe_service_calls(collector.on_service_call))
    await asyncio.sleep(0.3)

    op_ids: list[str] = []
    base = f"burst-{int(time.time())}"
    for i, group in enumerate(groups):
        data = kv_dict(common)
        data.update(kv_dict(group.split(",")))
        op_id = data.setdefault("op_id", f"{base}-{chr(ord('a') + i)}")
        op_ids.append(op_id)
        collector.expect(op_id)
        payload = coerce(service, data)
        print(f"-> {service_name}({json.dumps(payload, sort_keys=True)})")
        await maybe_await(client.execute_service(service, payload))
        await asyncio.sleep(0.2)

    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline and not all(collector.count(o) for o in op_ids):
        await asyncio.sleep(0.5)

    await asyncio.sleep(linger)
    print("\nsummary:")
    bad = collector.report(op_ids)
    await maybe_await(client.disconnect())
    return 1 if bad else 0



# ---------------------------------------------------------------------------
# upload subcommand (bulk schedule upload)
# ---------------------------------------------------------------------------

def _canonical_hash(entries: list[tuple[int, ...]], enabled: bool) -> str:
    """Python mirror of schedule_codec's canonical hash."""
    grid = {}
    for layer, day, sh, sm, eh, em in entries:
        grid[(layer, day)] = (sh, sm, eh, em)
    buf = bytearray()
    for layer in range(5):
        for day in range(7):
            cell = grid.get((layer, day))
            if cell is None:
                buf += bytes(6)
            else:
                buf += bytes([0x01, 0x02, *cell])
    buf.append(0x01 if enabled else 0x00)
    h = 0xCBF29CE484222325
    for b in buf:
        h = ((h ^ b) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return f"v1:{h:016x}"


def build_upload(enabled: str, entry_args: list[str]) -> tuple[str, str]:
    """Build the v1 payload + expected hash from CLI entry specs.

    Each entry spec is "layer,day,sh,sm,eh,em" (same fields as the wire
    grammar). Returns (payload, expected_hash); expected hash is only
    meaningful when enabled is "0"/"1" (not "-").

    Validation mirrors schedule_codec's parse rules, so a spec the firmware
    would reject dies here instead of printing a misleading expected hash.
    """
    if enabled not in ("0", "1", "-"):
        die(f"enabled flag must be 0, 1 or -: {enabled!r}")
    if len(entry_args) > 35:
        die("more than 35 entries")
    entries = []
    seen: set[tuple[int, int]] = set()
    for spec in entry_args:
        try:
            fields = [int(x) for x in spec.split(",")]
        except ValueError:
            die(f"non-numeric field in entry: {spec!r}")
        if len(fields) != 6:
            die(f"entry needs 6 comma-separated fields: {spec!r}")
        layer, day, sh, sm, eh, em = fields
        if not 0 <= layer <= 4:
            die(f"layer must be 0-4: {spec!r}")
        if not 0 <= day <= 6:
            die(f"day must be 0-6: {spec!r}")
        if not (0 <= sh <= 23 and 0 <= eh <= 23):
            die(f"hour must be 0-23: {spec!r}")
        if not (0 <= sm <= 59 and 0 <= em <= 59):
            die(f"minute must be 0-59: {spec!r}")
        if sh * 60 + sm >= eh * 60 + em:
            die(f"start must precede end (same-day interval): {spec!r}")
        if (layer, day) in seen:
            die(f"duplicate (layer, day) cell: {spec!r}")
        seen.add((layer, day))
        entries.append(tuple(fields))
    payload = ";".join([f"v1,{enabled}"] + [",".join(str(f) for f in e) for e in entries])
    expected = _canonical_hash(entries, enabled == "1") if enabled in ("0", "1") else "(n/a)"
    return payload, expected


def main() -> int:
    host, key, args = resolve_connection(sys.argv[1:])
    if not args:
        print(__doc__)
        return 2
    cmd, args = args[0], args[1:]

    timeout, linger = 30.0, 4.0
    rest: list[str] = []
    groups: list[str] = []
    it = iter(args)
    for a in it:
        if a == "--timeout":
            timeout = float(next(it, "30"))
        elif a == "--linger":
            linger = float(next(it, "4"))
        elif a == "--each":
            groups.append(next(it, ""))
        else:
            rest.append(a)

    if cmd == "services":
        return asyncio.run(cmd_services(host, key))
    if cmd == "states":
        return asyncio.run(cmd_states(host, key))
    if cmd == "call":
        if not rest:
            die("call requires a service name")
        return asyncio.run(cmd_call(host, key, rest[0], rest[1:], timeout, linger))
    if cmd == "upload":
        # upload <enabled 0|1|-> [layer,day,sh,sm,eh,em ...]
        if not rest:
            die("upload requires the enabled flag (0|1|-) first")
        payload, expected = build_upload(rest[0], rest[1:])
        print(f"payload:       {payload}")
        print(f"expected hash: {expected}")
        return asyncio.run(cmd_call(host, key, "upload_schedule",
                                    [f"data={payload}"], max(timeout, 160.0), linger))
    if cmd == "chain":
        # chain <service> [k=v ...] -- <service> [k=v ...] -- ...
        calls: list[list[str]] = []
        cur: list[str] = []
        for a in rest:
            if a == "--":
                if cur:
                    calls.append(cur)
                cur = []
            else:
                cur.append(a)
        if cur:
            calls.append(cur)
        if not calls:
            die("chain requires at least one service name")
        return asyncio.run(cmd_chain(host, key, calls, timeout, linger))
    if cmd == "burst":
        if not rest or not groups:
            die("burst requires a service name and at least one --each group")
        return asyncio.run(cmd_burst(host, key, rest[0], rest[1:], groups, timeout, linger))
    die(f"unknown command {cmd!r}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
