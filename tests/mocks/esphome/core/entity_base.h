#pragma once
#include <string>

// Mock of esphome/core/entity_base.h -- only the surface the host-compiled
// components touch.
//
// The one behaviour that matters here is has_state(): ESPHome uses it to mean
// "this entity has published at least once", NOT "the value is fresh". That
// distinction is load-bearing for dhw_demand (issue #149), so the mock has to
// get it right or the tests would be asserting against a friendlier world than
// the firmware runs in. set_has_state() is flipped by the publish paths below,
// never by a read.
namespace esphome {

class EntityBase {
 public:
  virtual ~EntityBase() = default;

  const std::string &get_name() const { return this->name_; }
  void set_name(const char *name) { this->name_ = name; }

  bool has_state() const { return this->has_state_; }
  void set_has_state(bool state) { this->has_state_ = state; }

 protected:
  std::string name_;
  bool has_state_{false};
};

}  // namespace esphome
