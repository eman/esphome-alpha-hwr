#pragma once
#include <string>
#include "esphome/core/component.h"
namespace esphome {
namespace sensor {
class Sensor : public Component {
public:
  void publish_state(float state) {}
};
}
}
