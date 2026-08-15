#!/usr/bin/env python3
"""Map a source file to the test targets that could possibly be affected by it.

The mutation check used to rebuild all 22 test binaries from scratch for every
one of its 54 mutations, which is where its wall-clock went -- the mutation
itself is a one-line edit. A mutation to `dhw_demand_logic.h` cannot change the
verdict of `test_auth`, so building and running it is pure waste.

The Makefile cannot answer this on its own: it lists *every* component header as
a prerequisite of *every* target, deliberately ("Coarse on purpose: a header
change rebuilds the suite, which takes seconds and is always correct"). That is
the right trade for a normal build and the wrong one when it is paid 54 times.
So the real include graph is taken from the compiler instead.

    make -B -n <targets>     -> the exact compile command per target
    g++ -MM <its .cpp files> -> every header those translation units include

which yields target -> {sources, headers}, inverted here into file -> targets.

**The failure mode is safe by construction.** Selecting too few targets makes a
mutation look like it SURVIVED -- a loud false alarm that stops the build.
Selecting too many only costs time. There is no arrangement in which this
reports a mutation as caught when the test that would catch it was not run, so
a bug here cannot manufacture confidence.

Usage:
    mutation_targets.py --build-map <cache.json>   # generate, once per run
    mutation_targets.py --lookup <cache.json> <file>  # print targets, one per line
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys

TESTS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tests")


def _targets() -> list[str]:
    """The TESTS list, straight from the Makefile so it cannot drift."""
    with open(os.path.join(TESTS_DIR, "Makefile"), encoding="utf-8") as fh:
        for line in fh:
            if line.startswith("TESTS ="):
                return line.split("=", 1)[1].split()
    raise SystemExit("mutation_targets: no TESTS list in tests/Makefile")


def _compile_commands(targets: list[str]) -> dict[str, list[str]]:
    """target -> the .cpp files its recipe compiles.

    `make -B -n` forces every rule to print without touching build state, so
    this is safe to run mid-session and does not invalidate anyone's binaries.
    """
    out = subprocess.run(
        ["make", "-B", "-n", *targets],
        cwd=TESTS_DIR, capture_output=True, text=True, check=False,
    ).stdout

    cmds: dict[str, list[str]] = {}
    for line in out.splitlines():
        if " -o " not in line:
            continue
        m = re.search(r"-o\s+(\S+)", line)
        if not m or m.group(1) not in targets:
            continue
        srcs = [tok for tok in line.split() if tok.endswith(".cpp")]
        if srcs:
            cmds[m.group(1)] = srcs
    return cmds


def _includes(srcs: list[str]) -> set[str]:
    """Every file the given translation units pull in, via the compiler."""
    res = subprocess.run(
        ["g++", "-std=c++17", "-I.", "-I./mocks", "-I../", "-MM", *srcs],
        cwd=TESTS_DIR, capture_output=True, text=True, check=False,
    )
    # -MM output is `target.o: a.cpp b.h \` continuation lines.
    body = res.stdout.replace("\\\n", " ")
    files = set()
    for line in body.splitlines():
        _, _, deps = line.partition(":")
        for tok in deps.split():
            if tok.endswith((".h", ".cpp", ".hpp")):
                files.add(os.path.normpath(os.path.join(TESTS_DIR, tok)))
    return files


def build_map(cache_path: str) -> None:
    targets = _targets()
    cmds = _compile_commands(targets)

    missing = [t for t in targets if t not in cmds]
    if missing:
        # Do not guess. A target whose recipe could not be read would silently
        # be excluded from every selection, which is the one way this could
        # under-select without anyone noticing.
        raise SystemExit(
            "mutation_targets: no compile command found for: " + " ".join(missing)
        )

    file_to_targets: dict[str, list[str]] = {}
    for target, srcs in cmds.items():
        deps = _includes(srcs)
        deps.update(os.path.normpath(os.path.join(TESTS_DIR, s)) for s in srcs)
        for f in deps:
            file_to_targets.setdefault(f, []).append(target)

    with open(cache_path, "w", encoding="utf-8") as fh:
        json.dump({k: sorted(v) for k, v in file_to_targets.items()}, fh)


def lookup(cache_path: str, path: str) -> None:
    with open(cache_path, encoding="utf-8") as fh:
        table = json.load(fh)
    key = os.path.normpath(os.path.abspath(path))
    for t in table.get(key, []):
        print(t)


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--build-map":
        build_map(sys.argv[2])
    elif len(sys.argv) == 4 and sys.argv[1] == "--lookup":
        lookup(sys.argv[2], sys.argv[3])
    else:
        raise SystemExit(__doc__)
