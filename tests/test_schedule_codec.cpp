// Host tests for the bulk-upload payload codec + canonical schedule hash
// (RFC-005 / dhw-sensor-apps issue #5).
//
// The golden hash vectors MUST stay identical to the Python suite in
// dhw-sensor-apps scheduler/tests/test_schedule_hash.py — they are the
// cross-language contract.

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "../components/alpha_hwr/schedule_codec.h"

int tests_passed = 0;
int tests_failed = 0;

#define TEST_ASSERT(condition, message) \
  if (condition) { \
    tests_passed++; \
    std::cout << "[PASS] " << message << std::endl; \
  } else { \
    tests_failed++; \
    std::cout << "[FAIL] " << message << std::endl; \
  }

namespace codec = esphome::alpha_hwr::codec;
using codec::LAYER_IMAGE_BYTES;
using codec::UPLOAD_LAYERS;
using codec::UploadRequest;

static bool parse(const std::string &data, UploadRequest *req) {
  std::string err;
  bool ok = codec::parse_upload_payload(data, req, &err);
  if (!ok) std::cout << "       parse error: " << err << std::endl;
  return ok;
}

static void test_parse_accepts() {
  UploadRequest req;
  TEST_ASSERT(parse("v1,1", &req), "empty enabled payload parses");
  TEST_ASSERT(req.entries.empty() && req.enabled == 1,
              "empty payload: no entries, enabled=1");

  TEST_ASSERT(parse("v1,0", &req) && req.enabled == 0,
              "disabled header parses");
  TEST_ASSERT(parse("v1,-", &req) && req.enabled == -1,
              "leave-untouched header parses");

  TEST_ASSERT(parse("v1,1;0,0,6,54,7,0;0,1,7,24,7,30;1,0,17,54,18,0", &req),
              "three-entry payload parses");
  TEST_ASSERT(req.entries.size() == 3, "three entries parsed");
  TEST_ASSERT(req.entries[0].layer == 0 && req.entries[0].day == 0 &&
                  req.entries[0].begin_hour == 6 &&
                  req.entries[0].begin_minute == 54 &&
                  req.entries[0].end_hour == 7 &&
                  req.entries[0].end_minute == 0,
              "first entry fields correct");
}

static void test_parse_rejects() {
  UploadRequest req;
  std::string err;

  TEST_ASSERT(!codec::parse_upload_payload("", &req, &err),
              "empty string rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v2,1", &req, &err),
              "unknown version rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,x", &req, &err),
              "bad enabled flag rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,0,6,54", &req, &err),
              "short entry rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;5,0,6,0,7,0", &req, &err),
              "layer 5 rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,7,6,0,7,0", &req, &err),
              "day 7 rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,0,24,0,25,0", &req, &err),
              "hour 24 rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,0,6,60,7,0", &req, &err),
              "minute 60 rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,0,7,0,7,0", &req, &err),
              "zero-length interval rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,0,8,0,7,0", &req, &err),
              "inverted interval rejected");
  TEST_ASSERT(
      !codec::parse_upload_payload("v1,1;0,0,6,0,7,0;0,0,8,0,9,0", &req, &err),
      "duplicate (layer, day) rejected");
  TEST_ASSERT(!codec::parse_upload_payload("v1,1;0,0,-1,0,7,0", &req, &err),
              "negative field rejected");

  // 36 entries: fill the whole grid + 1 duplicate is impossible, so build
  // 36 syntactically distinct entries by using an invalid extra one after
  // filling all 35 cells — the count check fires first.
  std::string payload = "v1,1";
  for (int layer = 0; layer < 5; layer++) {
    for (int day = 0; day < 7; day++) {
      char buf[32];
      snprintf(buf, sizeof(buf), ";%d,%d,6,0,7,0", layer, day);
      payload += buf;
    }
  }
  payload += ";0,0,8,0,9,0";  // 36th
  TEST_ASSERT(!codec::parse_upload_payload(payload, &req, &err),
              "more than 35 entries rejected");
}

static void test_layer_image() {
  UploadRequest req;
  parse("v1,1;2,3,6,30,7,15", &req);
  uint8_t image[LAYER_IMAGE_BYTES];

  codec::build_layer_image(req, 2, image);
  const uint8_t expected_cell[6] = {0x01, 0x02, 6, 30, 7, 15};
  TEST_ASSERT(memcmp(image + 3 * 6, expected_cell, 6) == 0,
              "entry lands in day-3 cell of layer 2");
  uint8_t zeros[6] = {0};
  TEST_ASSERT(memcmp(image + 0 * 6, zeros, 6) == 0,
              "other days are zero-filled");

  codec::build_layer_image(req, 0, image);
  bool all_zero = true;
  for (size_t i = 0; i < LAYER_IMAGE_BYTES; i++) {
    all_zero = all_zero && image[i] == 0;
  }
  TEST_ASSERT(all_zero, "unrelated layer image is all zeros");
}

static void test_canonicalize() {
  uint8_t image[LAYER_IMAGE_BYTES] = {0};
  // Disabled cell with stale times must zero out entirely
  image[0 * 6 + 0] = 0x00;
  image[0 * 6 + 2] = 9;  // stale begin hour
  image[0 * 6 + 4] = 10;
  // Enabled cell with a nonstandard action byte gets normalized
  image[1 * 6 + 0] = 0x01;
  image[1 * 6 + 1] = 0x07;
  image[1 * 6 + 2] = 6;
  image[1 * 6 + 4] = 7;

  codec::canonicalize_layer_image(image);
  uint8_t zeros[6] = {0};
  TEST_ASSERT(memcmp(image, zeros, 6) == 0,
              "stale times in disabled cell zeroed");
  TEST_ASSERT(image[1 * 6 + 1] == 0x02,
              "action byte normalized to 0x02 on enabled cell");
}

static void test_layer_image_to_json() {
  uint8_t image[LAYER_IMAGE_BYTES];
  memset(image, 0, sizeof(image));
  TEST_ASSERT(codec::layer_image_to_json(image) == "[0,0,0,0,0,0,0]",
              "empty layer renders 7 zero cells");

  UploadRequest req;
  parse("v1,1;2,0,6,54,7,0;2,6,23,54,23,59", &req);
  codec::build_layer_image(req, 2, image);
  TEST_ASSERT(codec::layer_image_to_json(image) ==
                  "[[414,420],0,0,0,0,0,[1434,1439]]",
              "cells render as [start_min,end_min]");

  // Worst case fits HA's 255-char state cap with plenty of room
  UploadRequest full;
  std::string payload = "v1,1";
  for (int day = 0; day < 7; day++) {
    payload += ";0," + std::to_string(day) + ",23,44,23,59";
  }
  parse(payload, &full);
  codec::build_layer_image(full, 0, image);
  std::string json = codec::layer_image_to_json(image);
  TEST_ASSERT(json.size() <= 255, "worst-case layer JSON fits 255 chars");
  TEST_ASSERT(json == "[[1424,1439],[1424,1439],[1424,1439],[1424,1439],"
                      "[1424,1439],[1424,1439],[1424,1439]]",
              "full layer renders all 7 cells");
}

static void test_golden_hash_vectors() {
  uint8_t images[UPLOAD_LAYERS][LAYER_IMAGE_BYTES];
  memset(images, 0, sizeof(images));

  TEST_ASSERT(codec::schedule_hash(images, false) == "v1:1410cfdeb1b46a77",
              "golden V1: empty grid, disabled");
  TEST_ASSERT(codec::schedule_hash(images, true) == "v1:1410cedeb1b468c4",
              "golden V2: empty grid, enabled");

  UploadRequest req;
  parse("v1,1;0,0,6,54,7,0;0,1,7,24,7,30;1,0,17,54,18,0", &req);
  for (uint8_t layer = 0; layer < UPLOAD_LAYERS; layer++) {
    codec::build_layer_image(req, layer, images[layer]);
  }
  TEST_ASSERT(codec::schedule_hash(images, true) == "v1:673dd2d1104de5b5",
              "golden V3: three-entry schedule");
}

static void test_hash_ignores_stale_disabled_times() {
  uint8_t a[UPLOAD_LAYERS][LAYER_IMAGE_BYTES];
  uint8_t b[UPLOAD_LAYERS][LAYER_IMAGE_BYTES];
  memset(a, 0, sizeof(a));
  memset(b, 0, sizeof(b));
  // b has stale times in a disabled cell
  b[2][4 * 6 + 2] = 13;
  b[2][4 * 6 + 4] = 14;
  TEST_ASSERT(codec::schedule_hash(a, true) == codec::schedule_hash(b, true),
              "stale disabled-cell times do not change the hash");
}

int main() {
  std::cout << "=== schedule_codec tests ===" << std::endl;
  test_parse_accepts();
  test_parse_rejects();
  test_layer_image();
  test_canonicalize();
  test_layer_image_to_json();
  test_golden_hash_vectors();
  test_hash_ignores_stale_disabled_times();
  std::cout << std::endl
            << "Passed: " << tests_passed << "  Failed: " << tests_failed
            << std::endl;
  return tests_failed == 0 ? 0 : 1;
}
