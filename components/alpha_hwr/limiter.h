#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#include "codec.h"

namespace esphome {
namespace alpha_hwr {
namespace services {

/**
 * The pump's flow limiters, and whether one of them is currently limiting.
 *
 * ## Why this exists (issue #274)
 *
 * The pump has a MaxFlow and a MinFlow limiter, separate from the setpoint,
 * settable from the Grundfos GO app's per-mode screens. This component read
 * none of it, and the consequence is the worst shape a diagnostic hole comes
 * in: **every signal we publish says the write worked, because it did.**
 *
 * Measured by @jfriend00 on a real installation, constant speed with MaxFlow
 * enabled at 1.6 gpm:
 *
 *     commanded 1700 -> delivered 1701     (tracking)
 *     commanded 1900 -> delivered 1903     (flow just reaching the cap)
 *     commanded 2000 -> delivered 1892     (holding)
 *     commanded 3000 -> delivered 1883     (63% of commanded)
 *
 * Every one of those writes settled `accepted`, and the pump reported the
 * commanded setpoint back from 86/7 each time -- its own answer, not a local
 * echo. Nothing in `Pump Run State`, in the settle event or in any entity said
 * the pump was not delivering it.
 *
 * `limiter_status_obj` exists precisely to answer this: a bool saying whether
 * that limiter is *currently* limiting, with the reference it is imposing
 * alongside it. That is the observable worth having, and it is what makes
 * issue #276's decision -- let the pump clamp, then explain -- completable:
 * "a limiter is active" is the one case where the slider is honest and the
 * pump still does not comply.
 *
 * ## The address map, swept rather than assumed
 *
 * | obj/sub | name | type | what |
 * | --- | --- | --- | --- |
 * | 86/600, 601 | `limiter_user_config_obj` | 895 v1 | enable + limit value |
 * | 86/620, 621 | `limiter_factory_config_obj` | 897 v1 | the settable bounds |
 * | 86/640, 641 | `limiter_status_obj` | 896 v1 | is it limiting, and at what |
 * | 86/660 | `limitation_manager_status_obj` | 896 v1 | which one is binding |
 *
 * The profile declares twenty slots per family. **They are empty.** Every
 * declared sub-id outside 600/601, 620/621 and 640/641 answers the same
 * nine-byte `OPERATION_FAILED` frame -- 602-619, 622-639 and 642-659, all
 * fifty-four, read on two pumps through two independent client
 * implementations. So the sub-id indexes the *limiter*, not the mode, and
 * reading anything but the six addresses above buys nothing.
 *
 * That matters for more than tidiness: the nine-byte refusal is below the
 * `len >= 11` gate in `Transport::try_dispatch_response()` and does not match
 * unless the caller sets `expect_short_ack`. A read of an absent sub-id is
 * therefore *invisible* and costs a full 3 s timeout. Both implementations hit
 * this independently. Do not sweep the range.
 *
 * ## Writing is deliberately not implemented
 *
 * Enabling a limiter silently caps the pump. That is not a change to make as a
 * side effect of a protocol sync, and #274 scopes it out explicitly: reading is
 * what closes the diagnostic hole.
 */

/// Which limiter a record describes. The pump's own `limiter_name` enum.
enum class LimiterName : uint8_t {
  NONE = 0x00,     ///< The manager object's "nothing is limiting" value.
  MAX_FLOW = 0x01,
  MIN_FLOW = 0x02,
};

inline const char *limiter_name_string(LimiterName n) {
  switch (n) {
    case LimiterName::MAX_FLOW: return "MaxFlow";
    case LimiterName::MIN_FLOW: return "MinFlow";
    case LimiterName::NONE:     return "none";
  }
  return "unknown";
}

/// `limiter_user_config_obj`, type 895 v1 (86/600, 86/601).
///
/// Payload is 18 bytes: `[limiter_name][enable][limit_value f32][kp][ti][td]`.
/// The three PID terms are read past but not kept -- the limiter behaves as a
/// control loop rather than a hard clamp (one sample reached 2.84 gpm against a
/// 1.6 gpm cap before settling back), which is presumably what they are for,
/// but nothing here acts on them.
struct LimiterConfig {
  bool valid{false};
  LimiterName name{LimiterName::NONE};
  bool enabled{false};
  float limit_m3s{0.0f};  ///< The cap, in the pump's native m³/s.

  /// The three PID floats that follow the cap in the record, kept as raw wire
  /// bytes (issue #299).
  ///
  /// Nothing here interprets them, and nothing should: they are kept so a WRITE
  /// can echo them back verbatim. Type 895 is one struct, so setting the cap
  /// means rewriting the whole record -- and a write that zeroed these would
  /// silently re-tune the pump's limiter loop, which is a far worse change than
  /// the one the caller asked for. `write_dhw_config()` echoes its stored
  /// setpoint bytes for exactly the same reason.
  ///
  /// Only meaningful when `valid`.
  uint8_t pid_raw[12]{};

  /// The same value in gallons per minute, which is the unit it was entered in.
  ///
  /// Not a convenience: the conversion is EXACT on every limit value seen, on
  /// two pumps. 3.5 gpm stores as 2.2081568e-04 m³/s and 1.6 gpm as
  /// 1.0094431e-04, matching to every digit the floats carry. Six of the seven
  /// flow floats on the bench pump are round gpm figures and are not round in
  /// m³/h, so gpm is the unit these were authored in and the one a user will
  /// recognise from the app.
  float limit_gpm() const { return limit_m3s * 15850.323f; }
};

/// `limiter_status_obj` (86/640, 641) and `limitation_manager_status_obj`
/// (86/660), both type 896 v1.
///
/// Payload is 6 bytes: `[limiter_name][limiter_status][limiter_reference f32]`.
struct LimiterStatus {
  bool valid{false};
  LimiterName name{LimiterName::NONE};
  bool limiting{false};   ///< Is this limiter constraining the pump right now?
  float reference{0.0f};  ///< A SPEED in RPM -- see the note below.

  /// `limiter_reference` is a **speed in RPM**, not a flow, and the magnitudes
  /// are what settle it. A flow in the pump's native m³/s would be around
  /// 1e-04; these read 1650, 1883 and 3671. Observed across three pump states:
  ///
  ///   stopped                     status 0, ref 0.0
  ///   running under the cap       status 0, ref 3671.0  (the range ceiling --
  ///                                                      i.e. no restriction)
  ///   commanded 3000, clamped     status 1, ref 1883.1  (measured motor speed
  ///                                                      at that moment: 1895)
  ///
  /// An enabled limiter with nothing to do publishes the top of the
  /// constant-speed range; one that is limiting publishes the speed it is
  /// imposing.
};

/// Bytes of payload each record carries, after the 3-byte size header.
static constexpr size_t LIMITER_CONFIG_BODY_LEN = 18;
/// Where the three PID floats start in the config record, and how many bytes
/// they occupy: `[name][enable][limit f32]` is six, leaving twelve (issue #299).
static constexpr size_t LIMITER_CONFIG_PID_OFFSET = 6;
static constexpr size_t LIMITER_CONFIG_PID_LEN = 12;
static constexpr size_t LIMITER_STATUS_BODY_LEN = 6;

/// Sub-ids. Named because "the sub-id indexes the limiter, not the mode" is the
/// fact a sweep of the whole declared range was needed to establish.
static constexpr uint16_t SUB_LIMITER_CONFIG_MAX_FLOW = 600;
static constexpr uint16_t SUB_LIMITER_CONFIG_MIN_FLOW = 601;
static constexpr uint16_t SUB_LIMITER_STATUS_MAX_FLOW = 640;
static constexpr uint16_t SUB_LIMITER_STATUS_MIN_FLOW = 641;
static constexpr uint16_t SUB_LIMITATION_MANAGER = 660;

inline LimiterName limiter_name_from_byte(uint8_t b) {
  switch (b) {
    case 0x01: return LimiterName::MAX_FLOW;
    case 0x02: return LimiterName::MIN_FLOW;
    default:   return LimiterName::NONE;
  }
}

/// Decode a type 895 v1 record. @p body starts after the 3-byte size header.
inline LimiterConfig decode_limiter_config(const uint8_t *body, size_t len) {
  LimiterConfig c{};
  // Two separate guards rather than one condition. Not only style: folding the
  // null check into a `&&` with the length check left cppcheck unable to prove
  // the pointer was non-null at the reads below, and it said so -- correctly,
  // since a reader has the same trouble. Keeping them apart also leaves the
  // length check on its own pipe-free line, which mutation_check.sh needs (it
  // splits its entries on that character).
  if (body == nullptr)
    return c;
  const bool long_enough = len >= LIMITER_CONFIG_BODY_LEN;
  if (!long_enough)
    return c;
  c.valid = true;
  c.name = limiter_name_from_byte(body[0]);
  c.enabled = body[1] != 0;
  c.limit_m3s = protocol::decode_float_be(body, 2);
  // Bytes 6..17: kp, ti, td. Copied rather than decoded -- a float round trip
  // is not guaranteed to reproduce the exact bytes, and the point of keeping
  // them is that a write puts back precisely what the pump had.
  for (size_t i = 0; i < LIMITER_CONFIG_PID_LEN; i++) {
    c.pid_raw[i] = body[LIMITER_CONFIG_PID_OFFSET + i];
  }
  return c;
}

/**
 * Build the eighteen-byte type 895 body for a limiter write (issue #299).
 *
 * Pure, and separated from the framing for the reason issue #282 established:
 * the guard below is otherwise reachable only through `ControlService`'s
 * private write primitive, which `WriteOperationService` calls after its own
 * validity check -- so the outer check masks this one, its mutation survives,
 * and the guard is protected rather than proven. CI found exactly that
 * (341/342, `limiter-write-without-a-read` surviving).
 *
 * What it guards is not cosmetic. The record is one struct, so setting the cap
 * rewrites all eighteen bytes, and bytes 6..17 are the pump's PID terms. With
 * no cached record there is nothing to echo, and writing zeros there would
 * re-tune the limiter's control loop invisibly -- a bigger change than the
 * caller asked for and one nothing would report.
 *
 * @param cached     The record as last read. Must be `valid`.
 * @param enabled    Whether the limiter should constrain the pump.
 * @param limit_m3s  The cap in the pump's native m3/s.
 * @param out        Receives LIMITER_CONFIG_BODY_LEN bytes. Untouched on false.
 * @return false when there is nothing to echo, and nothing may be written.
 */
inline bool build_limiter_write_body(const LimiterConfig &cached, bool enabled,
                                     float limit_m3s, uint8_t *out) {
  if (out == nullptr)
    return false;
  if (!cached.valid)
    return false;

  // The limiter's NAME is echoed from the pump's own byte rather than derived
  // from the sub-id. They agree on this pump; only the pump's byte is evidence
  // of that on a unit nobody here has seen.
  out[0] = static_cast<uint8_t>(cached.name);
  out[1] = enabled ? 0x01 : 0x00;
  protocol::encode_float_be(limit_m3s, &out[2]);
  for (size_t i = 0; i < LIMITER_CONFIG_PID_LEN; i++) {
    out[LIMITER_CONFIG_PID_OFFSET + i] = cached.pid_raw[i];
  }
  return true;
}


/// Decode a type 896 v1 record. @p body starts after the 3-byte size header.
inline LimiterStatus decode_limiter_status(const uint8_t *body, size_t len) {
  LimiterStatus s{};
  if (body == nullptr)
    return s;
  const bool long_enough = len >= LIMITER_STATUS_BODY_LEN;
  if (!long_enough)
    return s;
  s.valid = true;
  s.name = limiter_name_from_byte(body[0]);
  s.limiting = body[1] != 0;
  s.reference = protocol::decode_float_be(body, 2);
  return s;
}

/// Everything read from the limiter family, and the one question a user asks
/// of it.
struct LimiterState {
  LimiterConfig max_flow{};
  LimiterConfig min_flow{};
  LimiterStatus max_flow_status{};
  LimiterStatus min_flow_status{};
  LimiterStatus manager{};

  /// True when the pump is being held below what it was asked for.
  ///
  /// The manager object is preferred: it names which limiter is responsible and
  /// changes its `limiter_name` from 0 to 1 when MaxFlow becomes the binding
  /// constraint, so it answers the question directly. The per-limiter records
  /// are the fallback for a firmware that does not answer 660.
  /// Driven from the STATUS registers, never from the enable flag -- and that
  /// is what makes this correct across modes for free (issue #274).
  ///
  /// The limiter does not apply to every mode. Bench-established: it binds in
  /// constant curve and constant pressure (flow follows the cap, not the
  /// setpoint -- a commanded 3000 RPM delivered 1885 RPM and 1.59 gpm against a
  /// 1.6 cap), is presumed to bind in temperature control (same shared value,
  /// impractical to test without manufacturing return-line conditions), and does
  /// NOT bind in cycle time -- a 2.0 gpm cycle setpoint was delivered in full
  /// against a MaxFlow of 1.4, sustained, while the limiter was demonstrably
  /// active in other modes the same afternoon. The manual agrees: §9.3.4 cycle
  /// time and §9.3.7 constant flow mention no flow limits where §9.3.1-9.3.3 do.
  ///
  /// A signal keyed on `enabled` would therefore have to carry a mode table and
  /// keep it in step with firmware -- and would be wrong in the more misleading
  /// direction in cycle time, reporting a setpoint held down while it is being
  /// delivered exactly. The pump already answers the question per poll, so this
  /// asks it rather than modelling it.
  bool limiting() const {
    if (manager.valid && manager.limiting)
      return true;
    if (max_flow_status.valid && max_flow_status.limiting)
      return true;
    if (min_flow_status.valid && min_flow_status.limiting)
      return true;
    return false;
  }

  /// Which limiter is limiting, or NONE.
  LimiterName limiting_name() const {
    if (manager.valid && manager.limiting && manager.name != LimiterName::NONE)
      return manager.name;
    if (max_flow_status.valid && max_flow_status.limiting)
      return LimiterName::MAX_FLOW;
    if (min_flow_status.valid && min_flow_status.limiting)
      return LimiterName::MIN_FLOW;
    return LimiterName::NONE;
  }

  /// True once anything has been read, so "no limiter active" can be told apart
  /// from "we have not looked".
  bool known() const {
    return max_flow.valid || min_flow.valid || manager.valid ||
           max_flow_status.valid || min_flow_status.valid;
  }

  /// True when a limiter is switched on, whether or not it is biting right now.
  /// Worth its own question: an enabled MaxFlow that is not currently limiting
  /// will start limiting the moment the setpoint goes up.
  bool any_enabled() const {
    const bool max_on = max_flow.valid && max_flow.enabled;
    const bool min_on = min_flow.valid && min_flow.enabled;
    return max_on || min_on;
  }

  /// True when BOTH configuration records have been read.
  ///
  /// The all-clear needs this and `any_enabled()` does not, and the asymmetry
  /// is the point. One enabled limiter is enough to say "a limiter is enabled";
  /// saying "no limiter is enabled" is a claim about *both*, so reading MaxFlow
  /// as disabled and never reaching MinFlow must not produce it. Records are
  /// published as they land (a five-read chain does not always complete), so
  /// that half-read state is reachable on any link that drops mid-chain --
  /// and a false all-clear is the exact reassurance issue #274 exists to
  /// remove.
  bool config_complete() const { return max_flow.valid && min_flow.valid; }
};

/**
 * The `Pump Flow Limiter` entity's text.
 *
 * Three distinct states, because collapsing them loses the thing that made this
 * worth building. "Not read" is not "no limiter". "Enabled but not limiting" is
 * not "limiting" -- but it is also not "nothing to see", because it is the
 * state that turns into limiting as soon as the setpoint rises.
 */
inline std::string format_limiter_state(const LimiterState &s) {
  if (!s.known())
    return "unknown";

  char buf[128];
  if (s.limiting()) {
    const LimiterName who = s.limiting_name();
    // The manager can report that something is limiting without naming it, and
    // "none limiting" is not a sentence. Say what is known -- the pump IS being
    // held down -- and be explicit that which limiter is doing it was not
    // reported, rather than printing the NONE enum's own name.
    if (who == LimiterName::NONE)
      return "A limiter is active (which one is not reported)";
    const LimiterConfig &cfg =
        (who == LimiterName::MIN_FLOW) ? s.min_flow : s.max_flow;
    if (cfg.valid && cfg.enabled) {
      snprintf(buf, sizeof(buf), "%s limiting at %.2f gpm",
               limiter_name_string(who), cfg.limit_gpm());
    } else {
      snprintf(buf, sizeof(buf), "%s limiting", limiter_name_string(who));
    }
    return std::string(buf);
  }

  if (s.any_enabled()) {
    // Name every enabled limiter, not just the first: both can be on, and a
    // user reading "MaxFlow enabled" while MinFlow is also on would be told
    // something true and incomplete.
    std::string out;
    if (s.max_flow.valid && s.max_flow.enabled) {
      snprintf(buf, sizeof(buf), "MaxFlow enabled at %.2f gpm", s.max_flow.limit_gpm());
      out = buf;
    }
    if (s.min_flow.valid && s.min_flow.enabled) {
      snprintf(buf, sizeof(buf), "MinFlow enabled at %.2f gpm", s.min_flow.limit_gpm());
      if (!out.empty())
        out += "; ";
      out += buf;
    }
    return out + " (not limiting)";
  }

  // Nothing is enabled among what we HAVE read. That is only an all-clear if
  // both records arrived; otherwise the unread one could be the enabled one.
  if (!s.config_complete())
    return "unknown";
  return "No limiter enabled";
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
