#pragma once
#include <cstdint>
extern uint32_t mock_millis;
inline uint32_t millis() { return mock_millis; }
