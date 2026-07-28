#pragma once

#include <cmath>
#include <string>

// Publish-on-change gates for the detector's own output entities (issue #129).
//
// Same asymmetry #127 found in the control entities, in this component's
// outputs: sensor::publish_state() and text_sensor::publish_state() notify the
// frontend unconditionally, while BinarySensor::publish_state() drops repeats
// inside StatefulEntityBase::set_new_state(). The API layer's DeferredBatch
// dedups only within one batch_delay window (100 ms), so at a 10 s tick every
// republish becomes its own state frame per subscriber. One tick therefore cost
// four frames — confidence, demand level, detection method, session duration —
// even with the node completely idle and none of the four values moving:
// 0.40 frames/s/subscriber of pure repetition, measured over a 495 s window.
//
// These are step-valued detector outputs, not measurements that genuinely
// drift: detection_method names the rule that fired, confidence and
// demand_level are flat between transitions, and session_duration is 0 for as
// long as nothing is drawing. Nothing is lost by suppressing the repeats —
// ESPHome sends every entity's current state on connect, so a fresh subscriber
// still gets the full picture.
//
// The comparison is against the value last handed to publish_state()
// (get_raw_state(), pre-filter) rather than a value remembered here, so the
// gate stays correct if something else publishes to the same entity and a
// user-configured filter chain can never make the gate suppress an input the
// filters would have treated as new.
//
// force_update is honoured as the deliberate escape hatch: it is ESPHome's own
// "record every publish even when the value repeats" flag, so a consumer that
// wants these entities as a per-tick heartbeat asks for it there rather than by
// removing the gate. (text_sensor has no equivalent flag, so detection_method
// is always gated.)

namespace esphome {
namespace dhw_demand {

/// NaN-aware change test for a float entity. An input that has dropped out
/// publishes NaN, which is a meaningful state, and NaN != NaN — so NaN is
/// compared by identity: the first NaN publishes, later ones don't.
inline bool float_state_differs(bool has_state, float published, float candidate) {
  if (!has_state) {
    return true;
  }
  if (std::isnan(published) || std::isnan(candidate)) {
    return std::isnan(published) != std::isnan(candidate);
  }
  return published != candidate;
}

/// Publish `value` to a `sensor` only when it would change the entity.
/// Returns true when the publish happened. Templated on the entity type so this
/// header stays includable by the host test without the entity SDK.
template<typename SensorT> bool publish_sensor_if_changed(SensorT *entity, float value) {
  if (!entity->get_force_update() &&
      !float_state_differs(entity->has_state(), entity->get_raw_state(), value)) {
    return false;
  }
  entity->publish_state(value);
  return true;
}

/// Same contract for a `text_sensor`; compares against the entity's current
/// text. No force_update equivalent exists on text_sensor.
template<typename TextSensorT> bool publish_text_sensor_if_changed(TextSensorT *entity, const char *value) {
  if (entity->has_state() && entity->get_raw_state() == value) {
    return false;
  }
  entity->publish_state(value);
  return true;
}

}  // namespace dhw_demand
}  // namespace esphome
