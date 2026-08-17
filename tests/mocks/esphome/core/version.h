#pragma once
#define ESPHOME_VERSION "2025.12.0"

#define VERSION_CODE(major, minor, patch) ((major) << 16 | (minor) << 8 | (patch))

/// Pinned below 2026.1.0 so the host build takes the get_compilation_time()
/// branch in alpha_hwr.cpp. Both branches are real; this picks one, and the
/// choice is arbitrary rather than meaningful.
#define ESPHOME_VERSION_CODE VERSION_CODE(2025, 12, 0)
