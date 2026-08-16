/**
 * Device Information Service for Grundfos ALPHA HWR Pumps
 * 
 * This service handles reading device identification and metadata:
 * - Product name
 * - Serial number
 * - Software version
 * - Hardware version
 * - BLE version
 * 
 * The service uses Class 7 string parameters to read device information
 * via authenticated BLE connection.
 * 
 * Architecture:
 * - Transport Layer: Handles BLE communication and raw packets
 * - Protocol Layer: Parses frames and encodes/decodes strings
 * - Service Layer (this): Coordinates operations and manages state
 * 
 * Reference: reference/alpha-hwr/src/alpha_hwr/services/device_info.py
 */

#pragma once

#include "esphome/core/log.h"
#include <functional>
#include <cstdint>
#include <cstddef>
#include <string>

namespace esphome {
namespace alpha_hwr {
namespace core {
class Transport;
class Session;
}

namespace services {

/**
 * Device Information Service
 * 
 * Reads device identification strings using Class 7 commands.
 * All strings are cached after first successful read.
 */
class DeviceInfoService {
 public:
  /**
   * Constructor
   * 
   * @param transport BLE transport layer for packet I/O
   */
  explicit DeviceInfoService(core::Transport &transport);

  /**
   * Destructor
   */
  ~DeviceInfoService() = default;

  /**
   * Read all device information strings asynchronously.
   * 
   * Reads the following Class 7 strings:
   * - String ID 1: Product name ("ALPHA HWR")
   * - String ID 9: Serial number
   * - String ID 50: Software version
   * - String ID 52: Hardware version
   * - String ID 58: BLE version
   * 
   * Results are cached internally and can be accessed via getters.
   * 
   * @param on_complete Callback function(bool success)
   * @return True if read requests were queued successfully
   * 
   * Protocol Notes:
   * - Uses Class 7 ReadString command: [0x07][0x01][StringID]
   * - Response format: [Frame Header][String Data...][CRC]
   * - Strings are UTF-8 encoded, null-terminated
   * 
   * Reference: device_info.py::read_detailed() lines 230-300
   */
  bool read_device_info_async(std::function<void(bool)> on_complete);

  /**
   * Get product name.
   * 
   * @return Product name string (e.g., "ALPHA HWR"), or empty string if not read yet
   */
  const std::string& get_product_name() const { return product_name_; }

  /**
   * Get serial number.
   *
   * @return Full serial number, or empty string if not read yet
   */
  const std::string& get_serial_number() const { return serial_number_; }

  /**
   * Get software version.
   * 
   * @return Software version string, or empty string if not read yet
   */
  const std::string& get_software_version() const { return software_version_; }

  /**
   * Get hardware version.
   * 
   * @return Hardware version string, or empty string if not read yet
   */
  const std::string& get_hardware_version() const { return hardware_version_; }

  /**
   * Get BLE version.
   * 
   * @return BLE version string, or empty string if not read yet
   */
  const std::string& get_ble_version() const { return ble_version_; }

  /**
   * Read operating statistics from pump.
   *
   * Reads Object 93, Sub-ID 1 (Type 248: operation_history_pump_obj):
   *   - start_count: Total number of pump starts (uint32 BE, offset 0)
   *   - operating_time: Total operating seconds (uint32 BE, offset 8) → hours
   *
   * @param on_complete Callback(bool success, uint32_t start_count, float operating_hours)
   *
   * Reference: device_info.py::read_statistics() lines 302-367
   */
  void read_statistics_async(std::function<void(bool, uint32_t, float)> on_complete);

 private:
  core::Transport &transport_;

  // Cached device information
  std::string product_name_;
  std::string serial_number_;
  std::string software_version_;
  std::string hardware_version_;
  std::string ble_version_;

  // State tracking for async reads
  int pending_reads_ = 0;
  int failed_reads_ = 0;
  std::function<void(bool)> completion_callback_;

  /**
   * Read a Class 7 string asynchronously.
   * 
   * @param string_id String ID to read (1=product, 9=serial, 50=sw, 52=hw, 58=ble)
   * @param on_complete Callback function(bool success, const char* value)
   * 
   * Protocol Notes:
   * - APDU: [0x07][0x01][StringID]
   * - Response: [STX][LEN][DST][SRC][0x07][Count][...STRING...][CRC]
   *   Six-byte header. Byte 5 counts the string bytes that follow; the string
   *   itself starts at byte 6. See issue #179 -- this was read as a seven-byte
   *   [Cmd][ID] header, which cost every string its first character.
   *
   * Reference: base.py::_read_class7_string() in the Python client -- but note
   * it slices response[7:-2] and therefore still has the off-by-one described
   * above. It is where this parser was ported from, not a source to check
   * against; its own recorded device-info values are truncated for this reason.
   */
  void read_class7_string_async(uint8_t string_id, 
                                std::function<void(bool, const char*)> on_complete);

  /**
   * Handle completion of a single string read.
   * 
   * Decrements pending_reads_ counter and calls completion_callback_ when all reads finish.
   */
  void on_string_read_complete();

  // Logging tag
  static constexpr const char* TAG = "alpha_hwr.device_info";
};

}  // namespace services
}  // namespace alpha_hwr
}  // namespace esphome
