#pragma once
#include "esphome/core/component.h"
namespace esphome {
namespace binary_sensor {
class BinarySensor : public Component {
public:
  void publish_state(bool state) {}
};
}
}
