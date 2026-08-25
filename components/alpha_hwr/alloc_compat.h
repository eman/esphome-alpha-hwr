#pragma once

/**
 * Where ESPHome's *allocating* string helpers live, across the core versions
 * this component supports (issue #306).
 *
 * `format_hex_pretty`'s `std::string` overloads moved from
 * `esphome/core/helpers.h` to `esphome/core/alloc_helpers.h` in ESPHome
 * 2026.5.0. `helpers.h` still forwards to it, but the forwarding include says
 * of itself:
 *
 *     // These functions have moved to alloc_helpers.h. External components
 *     // should update their includes to use alloc_helpers.h directly.
 *     // Remove this comment and the template overload below before 2026.11.0
 *
 * So neither header works on its own across the supported range: including
 * `helpers.h` for it stops compiling once that shim is removed, and including
 * `alloc_helpers.h` unconditionally breaks every core older than 2026.5.0 --
 * where the file does not exist at all. `packages/README.md` declares a floor
 * of ESPHome 2024.6.0, so both ends have to keep building.
 *
 * Hence the version guard, which is the same idiom `alpha_hwr.cpp` already uses
 * for the `get_build_time_string()` split at 2026.1.0. It lives in one header
 * rather than at each of the three call sites so there is one place to delete
 * when the floor eventually rises above 2026.5.0 -- at which point this whole
 * file becomes `#include "esphome/core/alloc_helpers.h"` and then nothing.
 *
 * Both arms are compiled in CI, which is the part worth checking before
 * trusting a guard like this: the ESP32-C3 firmware job builds against the
 * latest core and takes the `alloc_helpers.h` arm, while the host test suite
 * mocks `ESPHOME_VERSION_CODE` at 2025.12.0 (`tests/mocks/esphome/core/
 * version.h`) and takes the `helpers.h` arm. Neither branch is untested, and
 * neither is a guess about a platform nobody builds.
 */

#include "esphome/core/version.h"

#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 5, 0)
#include "esphome/core/alloc_helpers.h"
#else
#include "esphome/core/helpers.h"
#endif
