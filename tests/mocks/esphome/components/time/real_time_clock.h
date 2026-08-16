#pragma once
// Mock of esphome/components/time/real_time_clock.h.
//
// The real header defines time::RealTimeClock; ESPTime and that class both live
// in the mock core/time.h, so this exists only to satisfy the include that
// time_service.h issues under USE_TIME. Building the write-op suite with
// -DUSE_TIME is what makes TimeService::current_time() a compiled, tested
// function rather than the three-line #else stub.
#include "esphome/core/time.h"
