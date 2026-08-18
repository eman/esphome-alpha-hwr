# Protocol Unit Tests

Host-based unit tests for the GENI protocol implementation. These tests verify the correctness of packet encoding, CRC calculations, and data parsing without requiring ESP32 hardware.

## Running Tests

```bash
cd tests
make test
```

### The object cache

Sources are compiled once per translation unit into `.obj/<flags-hash>/<group>/`
and linked from there, so editing one header recompiles the units that include
it rather than every target that might. Before this, a full build compiled 142
translation units out of ~40 distinct files.

Two things follow that are worth knowing:

- **Objects are keyed by the compiler and flags that produced them**, because
  make cannot see that flags changed. `make OPT=-O0`, `make test-asan` and
  `make test-clang` each get their own cache and coexist rather than evicting
  each other. The binaries are *not* keyed, so switching variants drops them and
  relinks — otherwise `make test` straight after `make test-asan` would re-run
  the sanitized build.
- **`-DUSE_TIME` and `-DUSE_TEXT_SENSOR` objects are kept apart** rather than
  pooled. Those defines change which code exists (AGENTS.md §4), so sharing an
  object between a target that sets one and a target that does not would report
  guards as covered that were never compiled.

`make clean` removes every variant. `make clean-bin` removes only the
executables, which is what `tools/mutation_check.sh` wants at the end of a run.

## Test Coverage

The test suite verifies:

- **CRC-16-CCITT Calculation**: Both base CRC and READ variant (with final XOR)
- **Class 10 Packet Encoding**: Register address encoding and packet structure
- **Big-Endian Float Decoding**: IEEE 754 float parsing from big-endian bytes
- **Packet Round-Trip**: End-to-end verification of packet encoding/decoding

## Files

### Source Files (Tracked by Git)
- `test_protocol.cpp` - Main test suite with all test cases
- `protocol.h` - Protocol implementation extracted for host testing
- `Makefile` - Build configuration
- `README.md` - This file

### Generated Files (Ignored by Git)
- `test_protocol` - Compiled test executable
- `*.o` - Object files
- `*.d` - Dependency files

### Temporary Test Utilities (Optional)
- `calc_test_crcs.cpp` - Helper to calculate CRC values for test vectors
- `verify_crc.cpp` - Standalone CRC verification utility

## Reference Implementation

The test vectors and expected values are verified against:
- Python reference: `reference/alpha-hwr/src/alpha_hwr/protocol/`
- Protocol docs: `reference/alpha-hwr/docs/protocol/wire_format.md`

## Test Results

Current status:
```
Tests passed: 35
Tests failed: 0

✓ ALL TESTS PASSED!
```

## Development Notes

When modifying the protocol implementation in `components/alpha_hwr/`:

1. **Do not copy it into the tests.** `tests/protocol.h` is a forwarding shim
   onto `codec.cpp` and `frame_builder.cpp`; it used to hold its own copy of the
   CRC table and packet builder, which meant the suite kept passing through a
   total protocol break. Anything added there must forward, never reimplement.
   Where a decision is awkward to reach from a test, extract it into a
   dependency-free header the way `response_match.h`, `pump_schedule_ux.h` and
   `dhw_demand_logic.h` do, and have production call that.
2. Run tests to verify correctness: `make test`
3. Run `./tools/mutation_check.sh` if you touched the protocol layer. It breaks
   production code on purpose and asserts the suite notices; a surviving
   mutation means a test is validating a replica rather than the shipped code.
   A full sweep of all 163 entries takes about 7 minutes; while iterating, pass
   a name filter (`./tools/mutation_check.sh gap-`) and let CI run the rest.
   **Commit before running it** — it restores its target files from HEAD after
   every mutation, so uncommitted edits to those files are silently discarded,
   and a run killed part-way can leave a mutation in the working tree.
4. Only commit source files (`.cpp`, `.h`, `Makefile`)
5. Do not commit compiled binaries or temporary files

## Why Host-Based Testing?

As per `AGENTS.md` requirements:

> Logic that does not depend on ESP hardware **MUST** be testable on the host machine.

This allows rapid iteration and verification without:
- Flashing ESP32 hardware
- Connecting to actual pump
- Waiting for BLE connections
- Manual verification of telemetry values

The protocol logic (CRC, packet encoding, float parsing) is deterministic and can be fully validated on the host.
