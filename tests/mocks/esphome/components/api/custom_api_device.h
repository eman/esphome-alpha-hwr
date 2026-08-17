#pragma once
//
// CustomAPIDevice stand-in that RECORDS AND REPLAYS instead of discarding.
//
// The previous version swallowed both calls, which was enough to link
// api_bridge.cpp but not to observe it -- and in fact nothing observed it at
// all: without the USE_API family in mocks/esphome/core/defines.h the whole
// translation unit compiled out to an empty object, so the file sat in
// test_component_wiring's link line contributing zero symbols. The comment in
// api_bridge.cpp::setup() said exactly this ("no host test builds it ...
// Pairing a handler with the wrong enumerator below compiles, passes the whole
// suite and passes the firmware build -- it surfaces only on a bench service
// listing or in somebody's automation").
//
// Three things are modelled deliberately:
//
//  * Registrations keep their NAME and ARGUMENT LIST, so a test can assert the
//    surface Home Assistant would see.
//  * Registrations also keep a type-erased INVOKER, built the same way the
//    real CustomAPIDevice builds one (static_cast<T *>(this) plus the member
//    pointer). That is what makes the name/handler pairing checkable: call a
//    service by the name it was registered under and see which command comes
//    back in the settle event.
//  * fire_homeassistant_event keeps every event in order. Ordering matters --
//    the exactly-one-terminal-event invariant and the "superseded fires early"
//    note are statements about the sequence, not just the contents.
//
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace esphome {
namespace api {

/// One service argument, in the only three types the bridge uses. ESPHome
/// service variables are bool/float/string here on purpose: int-typed ones hit
/// an ESP32-C3 RISC-V linker bug (see packages/alpha_hwr_schedule_editor.yaml).
struct ServiceArg {
  enum Kind { BOOL, FLOAT, STRING } kind;
  bool b{false};
  float f{0.0f};
  std::string s;

  ServiceArg(bool v) : kind(BOOL), b(v) {}                       // NOLINT
  ServiceArg(float v) : kind(FLOAT), f(v) {}                     // NOLINT
  ServiceArg(double v) : kind(FLOAT), f(static_cast<float>(v)) {}  // NOLINT
  ServiceArg(int v) : kind(FLOAT), f(static_cast<float>(v)) {}   // NOLINT
  ServiceArg(const char *v) : kind(STRING), s(v) {}              // NOLINT
  ServiceArg(std::string v) : kind(STRING), s(std::move(v)) {}   // NOLINT
};

template<typename P> P mock_convert_arg(const ServiceArg &a);
template<> inline bool mock_convert_arg<bool>(const ServiceArg &a) { return a.b; }
template<> inline float mock_convert_arg<float>(const ServiceArg &a) { return a.f; }
template<> inline std::string mock_convert_arg<std::string>(const ServiceArg &a) { return a.s; }

template<typename T, typename... Ts, std::size_t... I>
void mock_invoke_impl(T *self, void (T::*cb)(Ts...), const std::vector<ServiceArg> &vals,
                      std::index_sequence<I...>) {
  (self->*cb)(mock_convert_arg<typename std::decay<Ts>::type>(vals[I])...);
}

struct MockServiceRegistration {
  std::string name;
  std::vector<std::string> args;
  std::function<void(const std::vector<ServiceArg> &)> invoke;
};

struct MockHomeAssistantEvent {
  std::string name;
  std::map<std::string, std::string> data;
};

/// Recorded process-wide: the bridge is a private member of the component, so
/// a test observes it through these rather than through the object.
inline std::vector<MockServiceRegistration> &mock_registered_services() {
  static std::vector<MockServiceRegistration> v;
  return v;
}
inline std::vector<MockHomeAssistantEvent> &mock_fired_events() {
  static std::vector<MockHomeAssistantEvent> v;
  return v;
}
inline void mock_api_reset() {
  mock_registered_services().clear();
  mock_fired_events().clear();
}

/// Call a service the way Home Assistant would: by the name it registered
/// under. Returns false if no such service exists, which is itself a finding.
inline bool mock_call_service(const std::string &name, const std::vector<ServiceArg> &args) {
  for (auto &reg : mock_registered_services()) {
    if (reg.name != name) continue;
    if (reg.args.size() != args.size()) return false;
    reg.invoke(args);
    return true;
  }
  return false;
}

class CustomAPIDevice {
 public:
  template<typename T, typename... Ts>
  void register_service(void (T::*cb)(Ts...), const std::string &name,
                        const std::vector<std::string> &args) {
    T *self = static_cast<T *>(this);
    auto invoker = [self, cb](const std::vector<ServiceArg> &vals) {
      mock_invoke_impl(self, cb, vals, std::index_sequence_for<Ts...>{});
    };
    mock_registered_services().push_back({name, args, invoker});
  }
  template<typename T>
  void register_service(void (T::*cb)(), const std::string &name) {
    T *self = static_cast<T *>(this);
    auto invoker = [self, cb](const std::vector<ServiceArg> &) { (self->*cb)(); };
    mock_registered_services().push_back({name, {}, invoker});
  }
  void fire_homeassistant_event(const std::string &name,
                                const std::map<std::string, std::string> &data) {
    mock_fired_events().push_back({name, data});
  }
};

}  // namespace api
}  // namespace esphome
