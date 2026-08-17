#pragma once

// Mirror what a real build of this component defines. USE_TIME alone left the
// host build compiling *less* of alpha_hwr.h than the firmware does -- every
// text-sensor accessor sits behind USE_TEXT_SENSOR, so those code paths were
// invisible to the host tests while shipping in the binary.
#define USE_TIME
#define USE_SENSOR
#define USE_BINARY_SENSOR
#define USE_TEXT_SENSOR
