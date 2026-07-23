/**
 * Bulk schedule payload codec + canonical schedule hash (RFC-005, issue #5
 * in dhw-sensor-apps).
 *
 * Pure functions with no ESPHome dependencies so they compile in the host
 * test suite unchanged.
 *
 * Payload grammar (v1):
 *   data     := "v1," enabled ( ";" entry )*
 *   enabled  := "0" | "1" | "-"        ("-" = leave schedule-enabled as-is)
 *   entry    := layer "," day "," sh "," sm "," eh "," em
 * Full-state semantics: the payload expresses the ENTIRE 7x5 grid; any
 * (layer, day) cell absent from the payload is cleared (disabled).
 *
 * Canonical hash (v1): FNV-1a 64 over 211 bytes —
 *   for layer 0..4, day 0..6:
 *     enabled cell  -> [0x01, 0x02, SH, SM, EH, EM]
 *     disabled cell -> [0x00 x 6]   (stale times MUST NOT affect the hash)
 *   then 1 byte schedule_enabled (0x00|0x01)
 * rendered as "v1:" + 16 lowercase hex chars.
 *
 * The Python scheduler (dhw-sensor-apps scheduler/schedule_hash.py) mirrors
 * this algorithm; golden vectors live in both test suites.
 */

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace esphome {
namespace alpha_hwr {
namespace codec {

static constexpr uint8_t UPLOAD_LAYERS = 5;
static constexpr uint8_t UPLOAD_DAYS = 7;
static constexpr uint8_t UPLOAD_MAX_ENTRIES = UPLOAD_LAYERS * UPLOAD_DAYS;
static constexpr size_t LAYER_IMAGE_BYTES = UPLOAD_DAYS * 6;  // 42

struct UploadEntry {
  uint8_t layer{0};
  uint8_t day{0};
  uint8_t begin_hour{0};
  uint8_t begin_minute{0};
  uint8_t end_hour{0};
  uint8_t end_minute{0};
};

struct UploadRequest {
  std::vector<UploadEntry> entries;  // validated, no duplicate (layer, day)
  int8_t enabled{-1};                // -1 = leave untouched, 0/1 = set
};

/**
 * Parse and validate a v1 bulk payload. On failure returns false and puts a
 * short human-readable reason in `err`; `out` is left unspecified.
 */
bool parse_upload_payload(const std::string &data, UploadRequest *out,
                          std::string *err);

/**
 * Build the desired 42-byte wire image (7 days x 6 bytes) for one layer
 * from the request. Days without an entry are all-zero (disabled).
 */
void build_layer_image(const UploadRequest &request, uint8_t layer,
                       uint8_t out[LAYER_IMAGE_BYTES]);

/** FNV-1a 64 over an arbitrary buffer. */
uint64_t fnv1a64(const uint8_t *data, size_t len);

/**
 * Canonical schedule hash over five 42-byte layer images + the
 * schedule-enabled flag. Images must already be canonical (disabled days
 * all-zero) — build_layer_image() and canonicalize_layer_image() produce
 * that form.
 */
std::string schedule_hash(const uint8_t images[UPLOAD_LAYERS][LAYER_IMAGE_BYTES],
                          bool schedule_enabled);

/**
 * Render a 42-byte layer image as compact JSON: an array of 7 cells,
 * each `[start_min,end_min]` (enabled) or `0` (disabled). Emitted per
 * layer so it always fits HA's 255-char state cap (~86 chars worst
 * case). Read-back path for external schedulers (dhw-sensor-apps issue #7).
 */
std::string layer_image_to_json(const uint8_t image[LAYER_IMAGE_BYTES]);

/**
 * Canonicalize a raw layer image in place: any day whose enabled byte is
 * zero is zero-filled entirely, and enabled days get their action byte
 * normalized to 0x02, so stale times in disabled cells never perturb the
 * hash.
 */
void canonicalize_layer_image(uint8_t image[LAYER_IMAGE_BYTES]);

}  // namespace codec
}  // namespace alpha_hwr
}  // namespace esphome
