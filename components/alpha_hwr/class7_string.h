#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/**
 * Class 7 device-info string decoding, as a pure function.
 *
 * This lived in a lambda inside `DeviceInfoService::read_class7_string_async()`,
 * which is private, so the only way to reach it was through a `Transport`. That
 * made its memory-safety guard **unprovable** (issue #282): `Transport::
 * on_notification()` gained a floor on the declared length in #278, and the two
 * guards then masked each other's mutations -- relaxing the inner one is
 * invisible because the outer one rejects the frame first, and removing the
 * outer one is invisible because the inner one still refuses to parse it. CI
 * found it as an equivalent mutant, 266/267 on #279.
 *
 * Pulled out here for the reason `schedule_codec`, `telemetry_decoder` and
 * `response_match.h` are: a host test can hand it a 7-byte frame directly, with
 * no transport in front of it, so the guard is proven rather than merely
 * protected by something else. Class 7 was the odd one out.
 *
 * Both guards stay. The transport's floor is about framing and this one is about
 * memory safety, and a unit that parses bytes should not have to assume anything
 * about who handed them over. Two protections masking each other is a
 * documentation problem, not a duplication problem.
 */

namespace esphome {
namespace alpha_hwr {
namespace protocol {

/**
 * Response frame layout (issue #179):
 *
 *     [STX][LEN][DST][SRC][0x07][Count][...STRING...][CRC_H][CRC_L]
 *       0    1    2    3     4     5      6 ..            len-2
 *
 * The header is six bytes, not seven: byte 5 is a byte count for the string that
 * follows, not a [Cmd][ID] pair. Every captured frame has `byte5 == len - 8` and
 * the first character of the string at byte 6 -- e.g.
 * `24 0E F8 E7 07 0A 41 4C 50 48 41 ...` is a 10-byte "ALPHA HWR\0" in an
 * 18-byte frame. Reading from byte 7 dropped the first character of all five
 * strings, which is what the two transcribed "Python fix" string rewrites were
 * papering over.
 */
constexpr size_t CLASS7_HEADER_LEN = 6;
constexpr size_t CLASS7_CRC_LEN = 2;

/**
 * The shortest frame that can carry a (zero-length) string.
 *
 * Load-bearing, and this is the whole subject of issue #282. `string_bytes`
 * below is a `size_t` difference, so a shorter frame wraps it to ~1.8e19 and the
 * copy loop reads far past the frame. The guard is what stops that.
 *
 * It used to be justified by "transport.cpp dispatches Class 3/7 on `len >= 5`,
 * so 5-, 6- and 7-byte frames do reach this callback". That premise died with
 * #278's length floor. The guard did not, because its job was never to
 * compensate for the transport: it is what makes this function safe to call with
 * arbitrary bytes, which is exactly what the tests now do.
 */
constexpr size_t CLASS7_MIN_FRAME_LEN = CLASS7_HEADER_LEN + CLASS7_CRC_LEN;

/**
 * Longest string this decoder will return.
 *
 * Inherited from the 128-byte stack buffer this code used to fill, and kept as a
 * deliberate policy now that the buffer is gone: the longest string this pump
 * sends is a 23-character version, and a frame claiming 245 characters of device
 * name is not one to allocate for. Truncation is reported rather than silent.
 */
constexpr size_t CLASS7_MAX_STRING_LEN = 127;

/// Why a Class 7 frame did or did not yield a string.
enum class Class7Status : uint8_t {
  OK,
  /// Shorter than a header plus a CRC -- the case the guard exists for.
  FRAME_TOO_SHORT,
  /// Byte 4 is not 0x07, so this is not a Class 7 reply at all.
  NOT_CLASS_7,
  /// No buffer at all. Distinct from FRAME_TOO_SHORT because the causes are
  /// different -- a failed read against a malformed one -- and the log is the
  /// only place anyone ever sees which happened.
  NO_DATA,
};

/// Human-readable status, for logs. Kept beside the enum so a new kind cannot be
/// added without a name.
inline const char *class7_status_name(Class7Status status) {
  switch (status) {
    case Class7Status::OK:
      return "ok";
    case Class7Status::FRAME_TOO_SHORT:
      return "frame too short";
    case Class7Status::NOT_CLASS_7:
      return "not a Class 7 reply";
    case Class7Status::NO_DATA:
      return "no data";
  }
  return "unknown";
}

/**
 * The decode's whole result, including what the caller needs in order to log.
 *
 * Richer than the `std::optional<std::string>` issue #282 sketches, because the
 * call site distinguishes three outcomes in its warnings and collapsing them
 * into "no string" would lose the one diagnostic that has already caught a real
 * layout error -- the count byte disagreeing with the frame.
 */
struct Class7StringResult {
  Class7Status status{Class7Status::FRAME_TOO_SHORT};
  /// Only meaningful when `status == OK`.
  std::string value;
  /// Byte 5 as the frame declares it; 0 when the frame is too short to have one.
  uint8_t declared_count{0};
  /// How many string bytes the frame's own length says are there.
  size_t string_bytes{0};
  /// `declared_count != string_bytes`, judged only when the frame was long
  /// enough for both to exist.
  bool count_disagrees{false};
  /// The string was longer than CLASS7_MAX_STRING_LEN and was cut.
  bool truncated{false};
};

/**
 * Decode a Class 7 string reply.
 *
 * Safe to call with anything, which is the point: `data` may be null and `len`
 * may be any value, including one that would make the header/CRC subtraction
 * wrap. Nothing is read from `data` unless the length checks have already
 * established the byte exists.
 */
inline Class7StringResult decode_class7_string(const uint8_t *data, size_t len) {
  Class7StringResult result;

  // Order matters. The length is tested BEFORE any byte is read and before the
  // subtraction below, so neither the class-byte read nor `string_bytes` can be
  // reached on a frame that cannot support them.
  //
  // Two early returns rather than one `||`, for two reasons that happen to
  // agree. tools/mutation_check.sh splits its entries on '|', so a search string
  // holding one is truncated and the line cannot be anchored. And hoisting a
  // null check into a combined boolean defeats cppcheck's null-dereference
  // analysis, which is worth more here than the saved line.
  if (data == nullptr) {
    result.status = Class7Status::NO_DATA;
    return result;
  }
  if (len < CLASS7_MIN_FRAME_LEN) {
    result.status = Class7Status::FRAME_TOO_SHORT;
    return result;
  }

  if (data[4] != 0x07) {
    result.status = Class7Status::NOT_CLASS_7;
    return result;
  }

  // Now provably safe: len >= 8, so this is at worst 0.
  const size_t string_bytes = len - CLASS7_HEADER_LEN - CLASS7_CRC_LEN;
  result.string_bytes = string_bytes;
  result.declared_count = data[5];

  // The count byte is NOT used to bound the copy -- the frame length already
  // does that, and trusting a radio-supplied count is an overread waiting to
  // happen from the other direction. It is compared only so that a pump whose
  // layout differs from the captures says so, instead of silently handing back a
  // shifted string the way this parser did for its whole life.
  result.count_disagrees = static_cast<size_t>(data[5]) != string_bytes;

  result.status = Class7Status::OK;
  if (string_bytes == 0) {
    // A success, not a failure: an empty field is a thing a pump may legitimately
    // hold, and reporting it as a failed read loses that distinction.
    return result;
  }

  const uint8_t *string_data = data + CLASS7_HEADER_LEN;
  size_t take = string_bytes;
  if (take > CLASS7_MAX_STRING_LEN) {
    take = CLASS7_MAX_STRING_LEN;
    result.truncated = true;
  }

  result.value.reserve(take);
  for (size_t i = 0; i < take; i++) {
    if (string_data[i] == 0) {
      break;  // Stop at the first null
    }
    result.value.push_back(static_cast<char>(string_data[i]));
  }

  // Trim trailing whitespace.
  while (!result.value.empty()) {
    const char last = result.value.back();
    const bool is_space = last == ' ' || last == '\t' || last == '\r' || last == '\n';
    if (!is_space) {
      break;
    }
    result.value.pop_back();
  }

  return result;
}

}  // namespace protocol
}  // namespace alpha_hwr
}  // namespace esphome
