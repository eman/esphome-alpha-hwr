/**
 * History Service Implementation
 *
 * Reads trend data (TrendData1B, Type 946) from Object 53 SubIDs 451-454.
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/history.py
 */

#include "history_service.h"
#include "frame_builder.h"
#include "codec.h"
#include "transport.h"
#include "session.h"

#include <cstring>
#include <cmath>
#include <memory>

namespace esphome {
namespace alpha_hwr {
namespace services {

static constexpr uint8_t OBJECT_TRENDS = 53;

// SubID → { name, unit, scale }
struct TrendConfig {
  uint16_t sub_id;
  const char *name;
  const char *unit;
  float scale;
};

static const TrendConfig TREND_CONFIGS[] = {
    {451, "Flow",         "m³/h", 0.1f},
    {452, "Head",         "m",    0.1f},
    {453, "Temperature",  "°C",   1.0f},
    {454, "Power-on Time","h",    1.0f},
};
static constexpr size_t NUM_TRENDS = sizeof(TREND_CONFIGS) / sizeof(TREND_CONFIGS[0]);

TrendData TrendData::from_bytes(const uint8_t *data) {
  TrendData t;
  t.current_value = protocol::decode_float_be(data);
  memcpy(t.last_10, data + 4, 10);
  t.next_counter = data[14];
  t.next_100_value = protocol::decode_float_be(data + 15);
  memcpy(t.last_100, data + 19, 10);
  return t;
}

HistoryService::HistoryService(core::Transport &transport, core::Session &session)
    : transport_(transport), session_(session) {}

void HistoryService::read_trends_async(
    std::function<void(bool, const std::vector<TrendSeries> &)> on_complete) {
  if (!session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read history: session not ready");
    if (on_complete) on_complete(false, std::vector<TrendSeries>{});
    return;
  }

  ESP_LOGI(TAG, "Reading %zu trend channels...", NUM_TRENDS);

  auto trends = std::make_shared<std::vector<TrendSeries>>();
  auto read_next = std::make_shared<std::function<void(size_t)>>();

  *read_next = [this, trends, on_complete, read_next](size_t idx) {
    if (idx >= NUM_TRENDS) {
      cached_trends_ = *trends;
      trends_cached_ = true;
      ESP_LOGI(TAG, "Read %zu trend channels", trends->size());
      if (on_complete) on_complete(true, cached_trends_);
      return;
    }

    const auto &cfg = TREND_CONFIGS[idx];
    uint16_t sub_id = cfg.sub_id;

    uint8_t apdu[5];
    apdu[0] = 0x0A;
    apdu[1] = 0x03;
    apdu[2] = OBJECT_TRENDS;
    apdu[3] = (sub_id >> 8) & 0xFF;
    apdu[4] = sub_id & 0xFF;

    uint8_t frame[64];
    size_t frame_len = protocol::build_geni_packet(0xE7, 0xF8, apdu, 5, frame);
    std::vector<uint8_t> packet(frame, frame + frame_len);

    // Use wildcard response matching — accept any non-register Class 10 response.
    // Timeout: 1500 ms — trend reads either respond immediately or not at all.
    transport_.send_command(packet, 0, 0,
        [this, idx, trends, on_complete, read_next, cfg](
            bool success, const uint8_t *payload, size_t payload_len) {
      if (success && payload_len >= 32) {  // 3 header + 29 data
        const uint8_t *data = payload + 3;
        TrendData td = TrendData::from_bytes(data);

        TrendSeries series;
        series.name = cfg.name;
        series.unit = cfg.unit;
        series.scale = cfg.scale;
        series.current_value = td.current_value;

        // Scale the uint8 values
        for (int i = 0; i < 10; i++) {
          series.recent_10.push_back(td.last_10[i] * cfg.scale);
          series.recent_100.push_back(td.last_100[i] * cfg.scale);
        }

        trends->push_back(series);
        ESP_LOGD(TAG, "Trend %s: current=%.2f %s", cfg.name, td.current_value, cfg.unit);
      } else {
        // Some pump models do not populate all trend channels (e.g. Temperature
        // trend may be absent on certain hardware revisions). Log at DEBUG so
        // it doesn't produce noise on every startup.
        ESP_LOGD(TAG, "Trend %s: no data (success=%d, len=%zu) — may not be supported by this pump",
                 cfg.name, success, payload_len);
      }
      (*read_next)(idx + 1);
    }, 1500);
  };

  (*read_next)(0);
}

std::string HistoryService::format_display() const {
  if (!trends_cached_ || cached_trends_.empty()) {
    return "No history data";
  }

  std::string result;
  for (const auto &s : cached_trends_) {
    char buf[120];
    if (std::isnan(s.current_value)) {
      snprintf(buf, sizeof(buf), "%s: -- %s", s.name.c_str(), s.unit.c_str());
    } else {
      snprintf(buf, sizeof(buf), "%s: %.1f %s",
               s.name.c_str(), s.current_value, s.unit.c_str());
    }

    if (!result.empty()) result += "\n";
    result += buf;

    // Add recent 10-cycle values summary
    if (!s.recent_10.empty()) {
      result += " [";
      for (size_t i = 0; i < s.recent_10.size(); i++) {
        if (i > 0) result += ",";
        char v[16];
        snprintf(v, sizeof(v), "%.1f", s.recent_10[i]);
        result += v;
      }
      result += "]";
    }
  }
  return result;
}

void HistoryService::read_cycle_timestamps_async(
    int count, std::function<void(bool, const std::vector<uint32_t> &)> on_complete) {
  if (!session_.is_ready()) {
    ESP_LOGE(TAG, "Cannot read cycle timestamps: session not ready");
    if (on_complete) on_complete(false, std::vector<uint32_t>{});
    return;
  }

  // SubID 13300 = 10-cycle, SubID 13301 = 100-cycle
  uint16_t sub_id = (count <= 10) ? 13300 : 13301;
  ESP_LOGI(TAG, "Reading %d-cycle timestamps (Object 88, Sub %u)...", count <= 10 ? 10 : 100, sub_id);

  // Object 88 = 0x58
  uint8_t apdu[5];
  apdu[0] = 0x0A;  // Class 10
  apdu[1] = 0x03;  // OpSpec: INFO read
  apdu[2] = 0x58;  // Object 88
  apdu[3] = (sub_id >> 8) & 0xFF;
  apdu[4] = sub_id & 0xFF;

  uint8_t frame[64];
  size_t frame_len = protocol::build_geni_packet(0xE7, 0xF8, apdu, 5, frame);
  std::vector<uint8_t> packet(frame, frame + frame_len);

  // Use wildcard matching for Object 88 responses
  transport_.send_command(packet, 0, 0,
      [this, count, on_complete](bool success, const uint8_t *payload, size_t payload_len) {
    std::vector<uint32_t> timestamps;

    if (!success || payload_len < 7) {
      ESP_LOGW(TAG, "Cycle timestamps read failed (success=%d, len=%zu)", success, payload_len);
      if (on_complete) on_complete(false, timestamps);
      return;
    }

    // Skip 3-byte header if present [00 00 XX]
    const uint8_t *data = payload;
    size_t data_len = payload_len;
    if (data_len > 3 && data[0] == 0x00 && data[1] == 0x00) {
      data += 3;
      data_len -= 3;
    }

    // Parse array of uint32 BE timestamps
    size_t num_timestamps = data_len / 4;
    for (size_t i = 0; i < num_timestamps; i++) {
      size_t offset = i * 4;
      if (offset + 4 <= data_len) {
        uint32_t ts = (uint32_t(data[offset]) << 24) | (uint32_t(data[offset + 1]) << 16) |
                      (uint32_t(data[offset + 2]) << 8) | uint32_t(data[offset + 3]);
        if (ts > 0) {
          // Apply year-2000 offset if needed (same as reference)
          if (ts < 946684800) {  // Before Jan 1, 2000
            ts += 946684800;
          }
          timestamps.push_back(ts);
        }
      }
    }

    ESP_LOGI(TAG, "Read %zu cycle timestamps", timestamps.size());
    if (on_complete) on_complete(true, timestamps);
  }, 5000);
}

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
