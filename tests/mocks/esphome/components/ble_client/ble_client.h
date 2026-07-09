#pragma once
#include <cstdint>
namespace esphome {
namespace ble_client {
class BLEClient {
public:
  virtual ~BLEClient() {}
  virtual void write_value(uint16_t handle, const uint8_t *data, uint16_t length, bool response = true) {}
};
}
}
