# Protocol Unit Tests

Host-based unit tests for the GENI protocol implementation. These tests verify the correctness of packet encoding, CRC calculations, and data parsing without requiring ESP32 hardware.

## Running Tests

```bash
cd tests
make test
```

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
