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
