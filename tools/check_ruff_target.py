#!/usr/bin/env python3
"""
Assert ruff's `target-version` still matches the oldest Python ESPHome supports.

Why this exists. `components/*/__init__.py` is not run by CI -- it is run by
ESPHome inside the *user's* install, which may be on any interpreter ESPHome
accepts. So ruff's pyupgrade rules have to target the FLOOR of that range, not
whatever CI happens to run. Point them at the runner and ruff will happily
rewrite a schema to syntax that raises SyntaxError for a user one release
behind, and nothing here would catch it.

That floor is ESPHome's to move, not ours, and when it moves the pin goes stale
silently: ruff keeps enforcing the old target, the tree keeps passing, and the
newer syntax simply stays unavailable. This turns that into a failed build.

Deliberately a CHECK and not an auto-fix. Raising the target is a decision with
consequences -- it lets ruff rewrite the schemas to syntax that a user on the
previous floor cannot run, and it should happen when someone decides to drop
that support, not as a silent side effect of an `esphome` upgrade in CI.

Usage:
  python3 tools/check_ruff_target.py [--ruff-toml ruff.toml]

Exits 0 when they agree, 1 when they do not, and 2 when the answer cannot be
determined (esphome not installed, metadata missing a lower bound, and so on) --
the last is separated because "I could not tell" is not the same as "they
disagree", and a version of this that conflated them would fail every build that
runs without esphome present.
"""

from __future__ import annotations

import argparse
import importlib.metadata as md
import re
import sys
import tomllib
from pathlib import Path

# ">=3.12.0", ">= 3.12", ">=3.12.0rc1" -- the lower bound is the only clause that
# matters here. `~=3.12` is not handled: ESPHome does not use it, and guessing at
# a form nobody publishes would be worse than reporting UNKNOWN.
_LOWER_BOUND = re.compile(r">=\s*(\d+)\.(\d+)")

EXIT_OK = 0
EXIT_MISMATCH = 1
EXIT_UNKNOWN = 2


def esphome_floor() -> tuple[int, int]:
    """(major, minor) of the oldest Python the installed ESPHome accepts."""
    try:
        requires = md.distribution("esphome").metadata.get("Requires-Python")
    except md.PackageNotFoundError:
        raise LookupError("esphome is not installed in this environment") from None
    if not requires:
        raise LookupError("esphome's metadata declares no Requires-Python")
    m = _LOWER_BOUND.search(requires)
    if not m:
        raise LookupError(f"no >= lower bound in Requires-Python: {requires!r}")
    return int(m.group(1)), int(m.group(2))


class MissingTarget(Exception):
    """ruff.toml has no target-version at all.

    Deliberately NOT a LookupError, so it does not land in the UNKNOWN bucket
    with "esphome is not installed". An absent pin is the exact condition this
    script exists to prevent -- ruff falls back to its own default, which is
    tied to ruff's release rather than to ESPHome and drifts with upgrades. It
    is a failure, not an inability to answer. (Caught in review by testing the
    path rather than assuming it: the first version of this returned UNKNOWN
    here, so deleting the pin would have passed CI silently.)
    """


def ruff_target(path: Path) -> str:
    with path.open("rb") as fh:
        data = tomllib.load(fh)
    target = data.get("target-version")
    if not target:
        raise MissingTarget(
            f"{path} sets no target-version. Without it ruff falls back to its own\n"
            f"default, which tracks ruff's release rather than ESPHome's supported\n"
            f"range -- so a ruff upgrade could start rewriting components/ to syntax\n"
            f"a supported ESPHome install cannot run. Add it back."
        )
    return str(target)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--ruff-toml", default="ruff.toml", type=Path)
    args = ap.parse_args()

    try:
        major, minor = esphome_floor()
        configured = ruff_target(args.ruff_toml)
    except MissingTarget as exc:
        print(f"MISSING: {exc}", file=sys.stderr)
        return EXIT_MISMATCH
    except LookupError as exc:
        print(f"UNKNOWN: {exc}", file=sys.stderr)
        print("Not failing the build on this -- see the module docstring.", file=sys.stderr)
        return EXIT_UNKNOWN

    expected = f"py{major}{minor}"
    if configured == expected:
        print(f"ok: ruff target-version = {configured}, matching ESPHome's floor of {major}.{minor}")
        return EXIT_OK

    print(
        f"MISMATCH: {args.ruff_toml} sets target-version = {configured!r}, but the\n"
        f"installed ESPHome supports Python {major}.{minor} and up, so the correct\n"
        f"value is {expected!r}.\n"
        f"\n"
        f"components/ runs inside the user's ESPHome install, so this pin has to\n"
        f"track ESPHome rather than the interpreter CI runs.\n"
        f"\n"
        f"If ESPHome raised its floor, update ruff.toml to {expected!r} -- and note\n"
        f"that doing so lets ruff rewrite the schemas to syntax a user on\n"
        f"{configured.removeprefix('py')[0]}.{configured.removeprefix('py')[1:]} cannot run, which is the point of deciding it\n"
        f"deliberately rather than having CI do it for you.",
        file=sys.stderr,
    )
    return EXIT_MISMATCH


if __name__ == "__main__":
    raise SystemExit(main())
