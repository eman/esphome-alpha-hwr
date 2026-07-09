#pragma once
#include "esphome/core/component.h"
namespace esphome {
namespace time {
class RealTimeClock : public Component {
public:
  void now() {}
};
}
}
