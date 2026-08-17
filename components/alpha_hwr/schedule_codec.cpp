#include "schedule_codec.h"

#include <cstdio>
#include <cstring>

namespace esphome {
namespace alpha_hwr {
namespace codec {

namespace {

// Split `s` on `delim` (no empty-token suppression).
std::vector<std::string> split(const std::string &s, char delim) {
  std::vector<std::string> out;
  size_t start = 0;
  while (true) {
    size_t pos = s.find(delim, start);
    if (pos == std::string::npos) {
      out.push_back(s.substr(start));
      return out;
    }
    out.push_back(s.substr(start, pos - start));
    start = pos + 1;
  }
}

// Strict non-negative integer parse (no sign, no whitespace).
bool parse_uint(const std::string &s, int *out) {
  if (s.empty() || s.size() > 3) return false;
  int value = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    value = value * 10 + (c - '0');
  }
  *out = value;
  return true;
}

}  // namespace

bool parse_upload_payload(const std::string &data, UploadRequest *out,
                          std::string *err) {
  UploadRequest req;
  auto fail = [&](const std::string &reason) {
    if (err != nullptr) *err = reason;
    return false;
  };

  std::vector<std::string> parts = split(data, ';');
  if (parts.empty()) return fail("empty payload");

  // Header: "v1,<enabled>"
  std::vector<std::string> header = split(parts[0], ',');
  if (header.size() != 2 || header[0] != "v1")
    return fail("unknown payload version: " + parts[0]);
  if (header[1] == "0") {
    req.enabled = 0;
  } else if (header[1] == "1") {
    req.enabled = 1;
  } else if (header[1] == "-") {
    req.enabled = -1;
  } else {
    return fail("enabled flag must be 0, 1 or -: " + header[1]);
  }

  if (parts.size() - 1 > UPLOAD_MAX_ENTRIES)
    return fail("more than 35 entries");

  bool seen[UPLOAD_LAYERS][UPLOAD_DAYS] = {};
  for (size_t i = 1; i < parts.size(); i++) {
    std::vector<std::string> f = split(parts[i], ',');
    if (f.size() != 6) return fail("entry needs 6 fields: " + parts[i]);
    int v[6];
    for (int j = 0; j < 6; j++) {
      if (!parse_uint(f[j], &v[j]))
        return fail("non-numeric field in entry: " + parts[i]);
    }
    if (v[0] > 4) return fail("layer must be 0-4: " + parts[i]);
    if (v[1] > 6) return fail("day must be 0-6: " + parts[i]);
    if (v[2] > 23 || v[4] > 23)
      return fail("hour must be 0-23: " + parts[i]);
    if (v[3] > 59 || v[5] > 59)
      return fail("minute must be 0-59: " + parts[i]);
    int begin = v[2] * 60 + v[3];
    int end = v[4] * 60 + v[5];
    // A window whose end is earlier than its start crosses midnight, and that
    // is legitimate: schedule_entry.h models it (crosses_midnight(), and a
    // duration that wraps), the single-entry service accepts it with no
    // ordering rule at all, and the pump stores and reports it back verbatim
    // -- bench-verified 2026-08-17, 22:00-02:00 written to an empty cell and
    // read back as [1320,120].
    //
    // Rejecting it here made the bulk path the only one that could not express
    // a window the rest of the system supports, and broke the documented
    // round-trip: read a grid containing such a cell, upload it back, and the
    // upload was refused. Only the degenerate zero-length case is rejected now.
    if (begin == end)
      return fail("start and end are the same minute: " + parts[i]);
    if (seen[v[0]][v[1]])
      return fail("duplicate (layer, day) cell: " + parts[i]);
    seen[v[0]][v[1]] = true;

    UploadEntry entry;
    entry.layer = static_cast<uint8_t>(v[0]);
    entry.day = static_cast<uint8_t>(v[1]);
    entry.begin_hour = static_cast<uint8_t>(v[2]);
    entry.begin_minute = static_cast<uint8_t>(v[3]);
    entry.end_hour = static_cast<uint8_t>(v[4]);
    entry.end_minute = static_cast<uint8_t>(v[5]);
    req.entries.push_back(entry);
  }

  *out = req;
  return true;
}

void build_layer_image(const UploadRequest &request, uint8_t layer,
                       uint8_t out[LAYER_IMAGE_BYTES]) {
  memset(out, 0, LAYER_IMAGE_BYTES);
  for (const auto &entry : request.entries) {
    if (entry.layer != layer) continue;
    uint8_t *cell = out + entry.day * 6;
    cell[0] = 0x01;  // enabled
    cell[1] = 0x02;  // action: run
    cell[2] = entry.begin_hour;
    cell[3] = entry.begin_minute;
    cell[4] = entry.end_hour;
    cell[5] = entry.end_minute;
  }
}

uint64_t fnv1a64(const uint8_t *data, size_t len) {
  uint64_t h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; i++) {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

std::string layer_image_to_json(const uint8_t image[LAYER_IMAGE_BYTES]) {
  std::string json;
  json.reserve(96);
  json += "[";
  char buf[24];
  for (uint8_t day = 0; day < UPLOAD_DAYS; day++) {
    if (day > 0) json += ",";
    const uint8_t *cell = image + day * 6;
    if (cell[0] != 0x00) {
      int start = cell[2] * 60 + cell[3];
      int end = cell[4] * 60 + cell[5];
      snprintf(buf, sizeof(buf), "[%d,%d]", start, end);
      json += buf;
    } else {
      json += "0";
    }
  }
  json += "]";
  return json;
}

void canonicalize_layer_image(uint8_t image[LAYER_IMAGE_BYTES]) {
  for (uint8_t day = 0; day < UPLOAD_DAYS; day++) {
    uint8_t *cell = image + day * 6;
    if (cell[0] == 0x00) {
      memset(cell, 0, 6);
    } else {
      cell[0] = 0x01;
      cell[1] = 0x02;
    }
  }
}

std::string schedule_hash(
    const uint8_t images[UPLOAD_LAYERS][LAYER_IMAGE_BYTES],
    bool schedule_enabled) {
  uint8_t canonical[UPLOAD_LAYERS * LAYER_IMAGE_BYTES + 1];
  for (uint8_t layer = 0; layer < UPLOAD_LAYERS; layer++) {
    memcpy(canonical + layer * LAYER_IMAGE_BYTES, images[layer],
           LAYER_IMAGE_BYTES);
    canonicalize_layer_image(canonical + layer * LAYER_IMAGE_BYTES);
  }
  canonical[sizeof(canonical) - 1] = schedule_enabled ? 0x01 : 0x00;

  uint64_t h = fnv1a64(canonical, sizeof(canonical));
  char buf[24];
  snprintf(buf, sizeof(buf), "v1:%016llx",
           static_cast<unsigned long long>(h));
  return std::string(buf);
}

}  // namespace codec
}  // namespace alpha_hwr
}  // namespace esphome
