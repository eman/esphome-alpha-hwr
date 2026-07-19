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
  write_bench.py burst <service> <common k=v ...> --each k=v[,k=v] [--each ...]

`call` auto-generates an op_id if none is passed. `burst` fires one call per
--each group (merged over the common args) back to back on a single
connection, which is how to exercise the queued-write supersede path.

Connection settings (flag overrides environment):
  --host / ALPHA_HWR_HOST          node address (e.g. hwr-pump.local)
  --key  / ALPHA_HWR_API_KEY       api encryption key (base64)
  --secrets <file>                 read the key from a secrets.yaml (api_key:)

Run with a Python that has aioesphomeapi available; the esphome venv works:
  venv/bin/python tools/write_bench.py --host hwr-pump.local --secrets secrets.yaml services

Examples:
  write_bench.py call pump_set_setpoint mode=constant_speed value=2000
  write_bench.py call set_schedule_entry data=3,1,6,0,8,0
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
from typing import Any

from aioesphomeapi import APIClient

EVENT = "esphome.alpha_hwr_write_settled"


def die(msg: str) -> "None":
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
        return self.waiters.setdefault(op_id, asyncio.Event())

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


async def cmd_call(host: str, key: str, service_name: str, kvs: list[str],
                   timeout: float, linger: float) -> int:
    client = await connect(host, key)
    service = await find_service(client, service_name)

    data = dict(kv.split("=", 1) for kv in kvs)
    op_id = data.setdefault("op_id", f"bench-{int(time.time())}")
    payload = coerce(service, data)

    collector = EventCollector()
    matched = collector.expect(op_id)
    await maybe_await(client.subscribe_service_calls(collector.on_service_call))
    await asyncio.sleep(0.3)

    t0 = time.monotonic()
    print(f"-> {service_name}({json.dumps(payload, sort_keys=True)})")
    await maybe_await(client.execute_service(service, payload))

    try:
        await asyncio.wait_for(matched.wait(), timeout)
        print(f"settled in {time.monotonic() - t0:.1f}s")
    except asyncio.TimeoutError:
        print(f"NO TERMINAL EVENT for op_id={op_id} within {timeout}s <-- CONTRACT VIOLATION")
        await maybe_await(client.disconnect())
        return 1

    await asyncio.sleep(linger)  # catch duplicate terminal events
    bad = collector.report([op_id])
    await maybe_await(client.disconnect())
    return 1 if bad else 0


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
        data = dict(kv.split("=", 1) for kv in common)
        data.update(kv.split("=", 1) for kv in group.split(",") if kv)
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
    if cmd == "burst":
        if not rest or not groups:
            die("burst requires a service name and at least one --each group")
        return asyncio.run(cmd_burst(host, key, rest[0], rest[1:], groups, timeout, linger))
    die(f"unknown command {cmd!r}")
    return 2


if __name__ == "__main__":
    sys.exit(main())
