#pragma once
#include <string>
#include "esphome/core/component.h"
namespace esphome {
namespace text_sensor {
class TextSensor : public Component {
public:
  void publish_state(const std::string &state) {}
};
}
}
