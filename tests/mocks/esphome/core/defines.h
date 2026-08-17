#pragma once

// Mirror what a real build of this component defines. USE_TIME alone left the
// host build compiling *less* of alpha_hwr.h than the firmware does -- every
// text-sensor accessor sits behind USE_TEXT_SENSOR, so those code paths were
// invisible to the host tests while shipping in the binary.
// Guarded: several test rules also pass -DUSE_TIME on the command line, and an
// unguarded redefinition is a warning that CI treats as a failure.
#ifndef USE_TIME
#define USE_TIME
#endif
#define USE_SENSOR
#define USE_BINARY_SENSOR
#define USE_TEXT_SENSOR

// The API bridge's three gates. Without these, api_bridge.h compiles the whole
// file out: it sat in test_component_wiring's link line producing an empty
// object with zero symbols, so "api_bridge.cpp is in a test target" was true
// and meaningless. The shipped packages enable all three, so this is what a
// real build compiles.
#define USE_API
#define USE_API_CUSTOM_SERVICES
#define USE_API_HOMEASSISTANT_SERVICES
