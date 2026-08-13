#pragma once

#include <cmath>
#include <string>

#include "esphome/core/helpers.h"  // optional<>

// Publish-on-change gates for the polled template control entities
// (issue #127).
//
// ESPHome's number::publish_state() and select::publish_state() fire their
// state callback unconditionally — unlike switch::publish_state(), which drops
// repeats through publish_dedup_. A template entity whose lambda re-derives a
// cached pump value therefore emits an API state frame to *every* subscriber on
// *every* update_interval, changed or not. The ten control entities in
// packages/alpha_hwr_controls.yaml put a permanent ~2.8 frame/s/subscriber
// floor on the API write path that way; with several subscribers attached at
// DEBUG, that steady pressure is what exhausted the heap inside
// APIOverflowBuffer::enqueue_iov and rebooted the node.
//
// Returning an empty optional from a template lambda skips the publish
// entirely (template_number.cpp: `if (val.has_value())`; template_select.cpp:
// update_lambda()), so the gates below hand the value back only when it would
// actually change the entity. They compare against the entity's own published
// state rather than a value remembered inside the lambda, so they stay correct
// even if something else publishes to the same entity.
//
// Note the pump's own state is polled and pushed by the component
// (control_state_poll_interval, issue #54); these entities only re-read that
// cache, so suppressing the unchanged repeats loses no information.

namespace esphome {
namespace alpha_hwr {

/// NaN-aware change test for `number` entities. NaN is a meaningful state here
/// ("this setpoint does not apply in the active control mode"), and NaN != NaN,
/// so NaN is compared by identity: the first NaN publishes, later ones don't.
inline bool number_state_differs(bool has_state, float published, float candidate) {
  if (!has_state)
    return true;
  if (std::isnan(published) || std::isnan(candidate))
    return std::isnan(published) != std::isnan(candidate);
  return published != candidate;
}

/// Gate for a polled template `number` lambda: returns `value` only when it
/// would change the entity, otherwise an empty optional (no publish, no frame).
/// Templated on the entity type so this header does not pull in number.h — the
/// C++ component has no number/select dependency of its own; only the YAML
/// lambdas use these.
template<typename NumberT> optional<float> publish_number_if_changed(NumberT *entity, float value) {
  if (!number_state_differs(entity->has_state(), entity->state, value))
    return {};
  return value;
}

/// Gate for a polled template `select` lambda. Same contract as
/// publish_number_if_changed(); compares against the entity's current option.
template<typename SelectT>
optional<std::string> publish_option_if_changed(SelectT *entity, const std::string &option) {
  if (entity->has_state() && entity->current_option() == option)
    return {};
  return option;
}

/// Publish `value` to a `text_sensor` only when it would change the entity.
/// Returns true when the publish happened.
///
/// text_sensor::publish_state() does not dedup either, so a component that
/// republishes the same string on every reconnect or every write settle costs
/// one API frame per subscriber for nothing. Same contract as the dhw_demand
/// gate of the same name; no force_update equivalent exists on text_sensor.
template<typename TextSensorT>
bool publish_text_sensor_if_changed(TextSensorT *entity, const std::string &value) {
  if (entity->has_state() && entity->get_raw_state() == value)
    return false;
  entity->publish_state(value);
  return true;
}

}  // namespace alpha_hwr
}  // namespace esphome
