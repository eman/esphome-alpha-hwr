#include "alpha_hwr.h"
#include "frame_parser.h"
#include "telemetry_decoder.h"

namespace esphome {
namespace alpha_hwr {

// Static method to validate if a BLE device is an ALPHA HWR pump
// Delegates to BLE Connection Manager
bool AlphaHwrComponent::is_alpha_hwr_device(
    const esp32_ble_tracker::ESPBTDevice &device) {
  return core::BLEConnectionManager::is_alpha_hwr_device(
      device, GRUNDFOS_COMPANY_ID, PRODUCT_FAMILY_ALPHA, PRODUCT_TYPE_HWR,
      GRUNDFOS_SERVICE_UUID);
}

void AlphaHwrComponent::setup() {
  ESP_LOGI(TAG, "================== Alpha HWR Component setup() called "
                "==================");

  this->link_boot_ms_ = millis();  // Pump Link Status: mark the startup window

  // Initialize BLE connection manager
  ble_manager_.set_ble_client(parent_);
  ble_manager_.set_pairing_enabled(pairing_enabled_);
  ble_manager_.set_pairing_status_sensor(pairing_status_sensor_);
  ble_manager_.set_service_uuid(GRUNDFOS_SERVICE_UUID);
  ble_manager_.set_characteristic_uuid(GENI_CHAR_UUID);
  ble_manager_.init_security();

  // Set BLE manager callbacks
  ble_manager_.set_scheduler_callback(
      [this](uint32_t delay_ms, std::function<void()> callback) {
        this->set_timeout(delay_ms, std::move(callback));
      });

  ble_manager_.set_connection_callback([this]() {
    // A connection is opening; the settle hold-off (if any) is complete — clear
    // it and cancel any pending settle timer so nothing lingers.
    this->reconnect_settling_ = false;
    this->reconnect_timer_armed_ = false;
    this->cancel_timeout("reconnect_settle");
    this->session_.on_connected();
    // Pump Link Status: record this connection-open.
    this->link_last_open_ms_ = millis();
    this->link_ever_opened_ = true;
    this->link_reached_ready_ = false;
    this->evaluate_link_status();
  });

  ble_manager_.set_disconnection_callback([this]() {
    // Cancel in-flight auth so its pending scheduler lambdas are invalidated
    // and do not fire against the next BLE connection.
    this->auth_.cancel();
    // Also cancel the pending stabilize-to-auth timer, so a disconnect inside
    // the stabilize window can't leave it to start auth against the next connection.
    this->cancel_timeout("hwr_auth_start");
    // Stop telemetry so the next auth-complete callback can restart it cleanly.
    this->telemetry_service_.stop();
    // Reset initial-read flag so device info, clock sync, etc. are re-fetched
    // after reconnect (pump may have rebooted).
    this->initial_data_read_done_ = false;
    // Invalidate pending initial-read-chain timers so a disconnect mid-chain
    // can't leave them to fire reads against the next connection (issue #18).
    // The chain re-runs fresh on reconnect, so nothing is lost.
    this->read_chain_gen_++;
    this->session_.on_disconnected();
    // Clears command queue, pending handlers, reassembly buffer, and FSM state.
    this->transport_.reset();

    // Pump Link Status: a connection ended. If it had reached READY this is a
    // clean drop of a good link (start fresh); otherwise it was a failed attempt.
    if (this->link_reached_ready_) {
      this->link_consecutive_failures_ = 0;
    } else {
      this->link_consecutive_failures_++;
    }
    this->link_reached_ready_ = false;
    this->evaluate_link_status();

    // Optional reconnect settle window: after a disconnect, hold off reconnection
    // and start the settle timer only once the pump REAPPEARS (see parse_device),
    // so a just-powered-up pump has time to be ready before encryption is
    // requested. A premature on-open encryption request into a not-ready pump can
    // fail with 0x61 and make ESP-IDF erase the bond. Timing the window from the
    // pump's reappearance (not from this disconnect) makes it independent of how
    // long the pump was powered off. Device-agnostic alternative to conditioning
    // encryption timing on the pump's firmware variant.
    //
    // Gate on being BONDED: the bond-loss risk exists only when reconnecting to a
    // bonded pump (that path requests encryption-on-open). An unbonded reconnect
    // is initial pairing — pump-initiated, with no bond to lose — so holding it
    // off would only slow pairing. esp_ble_get_bond_device_num() > 0 is the same
    // bond test BLEConnectionManager::check_is_bonded() starts with; for this
    // single-pump node it means "the pump is bonded."
    if (this->reconnect_settle_ms_ > 0 && this->parent_ != nullptr &&
        esp_ble_get_bond_device_num() > 0) {
      ESP_LOGI(TAG, "Disconnected; holding reconnect until pump reappears + %u ms",
               this->reconnect_settle_ms_);
      this->parent_->set_auto_connect(false);
      this->reconnect_settling_ = true;
      this->reconnect_timer_armed_ = false;
      this->cancel_timeout("reconnect_settle");
    }
  });

  // NOTE: listener registration (so parse_device() receives scan results for the
  // reconnect-settle reappearance timing) is done at codegen in __init__.py via
  // esp32_ble_tracker.register_ble_device(), guarded on reconnect_settle_time > 0.

  ble_manager_.set_service_found_callback(
      [this]() { this->session_.on_service_found(); });

  ble_manager_.set_subscribed_callback([this]() {
    this->session_.on_subscribed();

    // Wait for pump to stabilize, then authenticate
    this->set_timeout("hwr_auth_start", 2000, [this]() {
      ESP_LOGI(TAG, "Pump stabilized. Starting authentication...");
      this->authenticate();
    });
  });

  ble_manager_.set_notification_callback(
      [this](const uint8_t *data, size_t len) {
        // Pass to transport for reassembly
        this->transport_.on_notification(data, len);
      });

  ble_manager_.set_advertisement_callback(
      [this](const core::PumpAdvertisementInfo &info) {
        ESP_LOGI(TAG, "Pump advertisement: family=0x%02X type=0x%02X version=0x%02X",
                 info.product_family, info.product_type, info.product_version);
#ifdef USE_TEXT_SENSOR
        if (product_version_sensor_ != nullptr) {
          char buf[5];
          snprintf(buf, sizeof(buf), "0x%02X", info.product_version);
          product_version_sensor_->publish_state(buf);
        }
#endif
      });

  // Set transport write callback
  this->transport_.set_write_callback(
      [this](const uint8_t *data, size_t len) -> bool {
        // Get GENI service and characteristic
        auto *service = this->parent_->get_service(GRUNDFOS_SERVICE_UUID);
        if (!service)
          return false;

        auto *chr =
            this->parent_->get_characteristic(service->uuid, GENI_CHAR_UUID);
        if (!chr)
          return false;

        auto status = esp_ble_gattc_write_char(
            this->parent_->get_gattc_if(), this->parent_->get_conn_id(),
            chr->handle, len, const_cast<uint8_t *>(data),
            ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        return (status == ESP_OK);
      });

  // Initialize transport callback for complete packets
  transport_.set_packet_callback([this](const uint8_t *data, size_t len) {
    // Route to telemetry service for processing
    this->telemetry_service_.on_packet(data, len);
  });

  // Initialize authentication module callbacks
  auth_.set_scheduler_callback(
      [this](uint32_t delay_ms, std::function<void()> callback) {
        this->set_timeout(delay_ms, std::move(callback));
      });

  auth_.set_completion_callback([this]() {
    this->session_.on_authenticated();
    ESP_LOGI(TAG, "✓ Authentication handshake complete - pump ready");

    // Start telemetry service when authenticated
    this->telemetry_service_.start();

    // Pump Link Status: we reached a working link.
    this->link_reached_ready_ = true;
    this->link_consecutive_failures_ = 0;
    this->evaluate_link_status();

    // Trigger the one-time data read chain
    this->trigger_initial_data_reads();
  });

  telemetry_service_.set_sensor_publisher(&sensor_publisher_);
  sensor_publisher_.setup_head_rate_callback();

  // Set control service reference in telemetry service for passive mode
  // notifications
  telemetry_service_.set_control_service(&control_service_);

  // Initialize control service callbacks
  control_service_.set_schedule_callback(
      [this](std::function<void()> callback, uint32_t delay_ms) {
        this->set_timeout(delay_ms, std::move(callback));
      });

  // Delegate config commits to ScheduleService which preserves the cached
  // ClockProgramOverview (including schedule_enabled flag)
  control_service_.set_config_commit_callback(
      [this]() { this->schedule_service_.send_configuration_commit(); });

  // Set control mode change callback to publish to text sensor
  control_service_.set_mode_change_callback([this](services::ControlMode mode,
                                                   uint8_t operation_mode,
                                                   float setpoint) {
#ifdef USE_TEXT_SENSOR
    // Only publish if the control service has a valid mode from the pump
    if (this->control_mode_sensor_ && this->control_service_.is_mode_valid()) {
      const char *mode_name = services::ControlService::get_mode_name(mode);
      this->control_mode_sensor_->publish_state(mode_name);
      ESP_LOGI(TAG, "Published control mode to sensor: %s", mode_name);
    }
#endif
  });

  // Initialize schedule service callbacks
  schedule_service_.set_schedule_callback(
      [this](std::function<void()> callback, uint32_t delay_ms) {
        this->set_timeout(delay_ms, std::move(callback));
      });

  schedule_service_.set_timeout_callback(
      [this](std::function<void()> callback, uint32_t delay_ms) {
        this->set_timeout(delay_ms, std::move(callback));
      });

  schedule_service_.set_state_change_callback(
      [this](bool enabled) { this->publish_schedule_json(); });

  // Initialize schedule text sensor with "Loading..." state
#ifdef USE_TEXT_SENSOR
  if (this->schedule_text_sensor_) {
    this->schedule_text_sensor_->publish_state("Loading schedule...");
  }

  // Control mode text sensor will be populated when we receive the passive
  // notification from the pump during authentication. Do NOT publish a
  // default/unknown value here.
#endif
}

bool AlphaHwrComponent::parse_device(
    const esp32_ble_tracker::ESPBTDevice &device) {
  // Passive observer used only to time the reconnect settle window from the
  // pump's reappearance. Always returns false (we never "claim" the device;
  // the ble_client owns the actual connection).
  if (!this->reconnect_settling_ || this->reconnect_timer_armed_ ||
      this->parent_ == nullptr) {
    return false;
  }
  if (device.address_uint64() != this->parent_->get_address()) {
    return false;
  }
  // Pump is advertising again after the disconnect — start the settle timer once.
  this->reconnect_timer_armed_ = true;
  ESP_LOGI(TAG, "Pump reappeared; holding reconnect %u ms to let it settle",
           this->reconnect_settle_ms_);
  this->set_timeout("reconnect_settle", this->reconnect_settle_ms_, [this]() {
    ESP_LOGI(TAG, "Reconnect settle window elapsed; allowing reconnect");
    this->reconnect_settling_ = false;
    this->reconnect_timer_armed_ = false;
    if (this->parent_ != nullptr) {
      this->parent_->set_auto_connect(true);
    }
  });
  return false;
}

void AlphaHwrComponent::loop() {
  // Process transport command queue and state machine
  this->transport_.loop();

  // Pump Link Status: periodic re-evaluation (throttled). This catches the
  // no-events "unreachable" case — when the pump is offline the connect loop
  // produces no callbacks, so only an elapsed-time check can detect it.
  if (millis() - this->link_last_eval_ms_ >= 1000) {
    this->link_last_eval_ms_ = millis();
    this->evaluate_link_status();
  }
}

// Pump Link Status state machine. The status is the FIRST matching condition
// below (a priority ladder), re-evaluated on the connection/disconnection/auth
// callbacks and on the ~1s loop() tick above:
//
//   Connected     the GENI session has reached READY (session_.is_ready()): the
//                 pump is fully usable, not merely BLE-linked.
//   Initializing  no connection has opened since boot, still within the 15s boot
//                 grace (LINK_INIT_GRACE_MS).
//   Unpaired      pairing is enabled but there was no bond at the last open (the
//                 pump has no stored bond: never paired yet, or the bond was
//                 erased by an encryption failure like 0x61). NOTE: this also
//                 shows transiently through an initial pairing handshake, since no
//                 bond exists until it completes, before flipping to Connected; it
//                 persists only when the bond is genuinely absent (needs re-pair).
//   Unreachable   no successful open for over 20s (LINK_UNREACHABLE_MS), measured
//                 from the last open / last Connected; or never opened since boot
//                 and past the 15s grace. Covers both an absent pump and a present
//                 pump we cannot connect to (the two are not distinguished).
//   Reconnecting  not ready, opened within 20s, but >= 3 consecutive failed
//                 attempts (LINK_FAIL_K): links keep opening yet the session keeps
//                 failing before READY.
//   Connecting    not ready, opened within 20s, fewer than 3 failures: a normal
//                 in-progress attempt (including the first after a clean drop).
void AlphaHwrComponent::evaluate_link_status() {
#ifdef USE_TEXT_SENSOR
  // Companion sensor: show the latched failure reason only while the link is unhealthy;
  // read "None" once it's Connected again, so a stale reason doesn't sit next to a healthy
  // status. "None" is also the pre-failure default.
  if (this->pump_last_link_failure_sensor_ != nullptr) {
    const std::string &lf = this->ble_manager_.get_last_failure();
    const std::string shown =
        (this->session_.is_ready() || lf.empty()) ? std::string("None") : lf;
    if (shown != this->link_last_failure_published_) {
      this->link_last_failure_published_ = shown;
      this->pump_last_link_failure_sensor_->publish_state(shown);
    }
  }

  if (this->pump_link_status_sensor_ == nullptr)
    return;

  constexpr uint32_t LINK_INIT_GRACE_MS = 15000;   // boot grace before "unreachable"
  constexpr uint32_t LINK_UNREACHABLE_MS = 20000;  // no open this long -> unreachable
  constexpr uint16_t LINK_FAIL_K = 3;              // failed attempts -> reconnecting
  const uint32_t now = millis();

  const char *state;
  if (this->session_.is_ready()) {
    state = "Connected";
    this->link_last_open_ms_ = now;  // measure "unreachable" from the drop, not the first open
  } else if (!this->link_ever_opened_) {
    state = (now - this->link_boot_ms_ < LINK_INIT_GRACE_MS) ? "Initializing"
                                                             : "Unreachable";
  } else if (this->pairing_enabled_ && !this->ble_manager_.was_bonded_at_open()) {
    state = "Unpaired";
  } else if (now - this->link_last_open_ms_ > LINK_UNREACHABLE_MS) {
    state = "Unreachable";
  } else if (this->link_consecutive_failures_ >= LINK_FAIL_K) {
    state = "Reconnecting";
  } else {
    state = "Connecting";
  }

  if (this->link_last_status_ != state) {
    this->link_last_status_ = state;
    this->pump_link_status_sensor_->publish_state(state);
    ESP_LOGI(TAG, "Pump link status: %s", state);
  }
#endif
}

void AlphaHwrComponent::trigger_initial_data_reads() {
  if (initial_data_read_done_)
    return;
  initial_data_read_done_ = true;
  ESP_LOGD(TAG, "Triggering initial data reads...");

  // Capture the current generation; each timer lambda below checks it so a
  // disconnect (which bumps read_chain_gen_) invalidates the whole chain.
  const uint32_t gen = this->read_chain_gen_;

  // Read device information strings
  this->set_timeout(1000, [this, gen]() {
    if (gen != this->read_chain_gen_) return;  // Stale: disconnected mid-chain
    ESP_LOGD(TAG, "Reading device information...");
    this->read_device_info();

    // Chain: read operating statistics after device info
    this->set_timeout(2000, [this, gen]() {
      if (gen != this->read_chain_gen_) return;
      ESP_LOGD(TAG, "Reading operating statistics...");
      this->read_statistics();
    });
  });

  // Sync pump clock (Read time first to calculate drift, then sync)
  this->set_timeout(2000, [this, gen]() {
    if (gen != this->read_chain_gen_) return;
    ESP_LOGD(TAG, "Performing initial pump clock sync...");

    // First read the pump clock to measure drift
    this->time_service_.get_clock_async([this](ESPTime pump_time) {
      if (pump_time.is_valid()) {
        time_t now = ::time(nullptr);
        // Calculate drift (Pump Time - System Time)
        // If pump is ahead, diff is positive. If pump is behind, diff is
        // negative.
        double diff = difftime(pump_time.timestamp, now);

        ESP_LOGI(TAG, "Pump clock drift before sync: %.0f seconds", diff);

        if (this->clock_diff_sensor_) {
          this->clock_diff_sensor_->publish_state(diff);
        }
      } else {
        ESP_LOGW(TAG, "Could not read pump clock for drift measurement");
        // Publish NAN to indicate invalid/unknown drift
        if (this->clock_diff_sensor_) {
          this->clock_diff_sensor_->publish_state(NAN);
        }
      }

      // Now sync the clock
      this->time_service_.set_clock_async([this](bool success) {
        if (success) {
          ESP_LOGD(TAG, "Initial pump clock sync successful");
          this->last_time_sync_timestamp_ = millis();

          // Update last sync time sensor
#ifdef USE_TEXT_SENSOR
          if (this->last_clock_sync_sensor_) {
            time_t now = ::time(nullptr);
            const struct tm *tm_info = localtime(&now);
            char buf[32];
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
            this->last_clock_sync_sensor_->publish_state(buf);
          }
#endif
        } else {
          ESP_LOGW(TAG,
                   "Initial pump clock sync failed - will retry in 24 hours");
        }
      });
    });
  });

  // Refresh schedule display
  this->set_timeout(4000, [this, gen]() {
    if (gen != this->read_chain_gen_) return;
    ESP_LOGD(TAG, "Refreshing schedule display...");
    this->update_schedule_display();
  });

  // Read event log, then chain history, then single events
  this->set_timeout(6000, [this, gen]() {
    if (gen != this->read_chain_gen_) return;
    ESP_LOGD(TAG, "Reading event log...");
    this->read_event_log([this, gen](bool success) {
      if (success) {
        ESP_LOGD(TAG, "Event log read complete");
      } else {
        ESP_LOGW(TAG, "Event log read failed");
      }
      this->set_timeout(2000, [this, gen]() {
        if (gen != this->read_chain_gen_) return;
        ESP_LOGD(TAG, "Reading history trends...");
        this->read_history([this, gen](bool success) {
          if (success) {
            ESP_LOGD(TAG, "History trends read complete");
          }
          this->set_timeout(2000, [this, gen]() {
            if (gen != this->read_chain_gen_) return;
            ESP_LOGD(TAG, "Reading single events...");
            this->read_single_events(
                [](bool success,
                   const std::vector<services::SingleEvent> &events) {
                  if (success) {
                    ESP_LOGD("alpha_hwr", "Read %zu active single events",
                             events.size());
                  }
                });
          });
        });
      });
    });
  });

  // Query control mode and setpoints
  this->set_timeout(5000, [this, gen]() {
    if (gen != this->read_chain_gen_) return;
    ESP_LOGD(TAG, "Reading control mode and setpoints from pump...");
    this->control_service_.read_setpoints_from_pump();
  });
}

// Called every 10 seconds by PollingComponent
void AlphaHwrComponent::update() {
  if (session_.is_ready() && parent_ && parent_->get_conn_id() != 0xFF) {
    // If session is ready but initial data reads haven't been triggered yet
    // (e.g., BLE connection persisted through ESP32 restart, no re-auth),
    // trigger them now.
    if (!initial_data_read_done_) {
      ESP_LOGD(TAG,
               "Session ready but initial data not yet read - triggering now");
      telemetry_service_.start();
      trigger_initial_data_reads();
    }

    // Poll telemetry first
    telemetry_service_.poll();

    // CRITICAL FIX: Space out schedule poll to avoid request collision
    // The pump appears to have trouble handling concurrent Class 10 reads.
    // Delay schedule poll by 500ms to ensure telemetry response completes
    // first.
    this->set_timeout("schedule_poll", 500,
                      [this]() { schedule_service_.poll_state(); });

    // Periodic control state polling (fixes #54): detect out-of-band pump state
    // changes (e.g., internal schedule execution, manual button press, external
    // app control). Reuses existing schedule_callback mechanism with a longer
    // interval to minimize BLE traffic.
    if (control_state_poll_interval_ms_ > 0) {
      uint32_t now = millis();
      
      // Handle millis() rollover (every ~49 days)
      if (now < last_control_state_poll_time_) {
        ESP_LOGD(TAG, "millis() rollover detected, resetting control state poll timer");
        last_control_state_poll_time_ = 0;
      }

      if (last_control_state_poll_time_ == 0 ||
          (now - last_control_state_poll_time_) >= control_state_poll_interval_ms_) {
        last_control_state_poll_time_ = now;
        
        // Schedule the control state readback after telemetry poll completes
        // (1000ms delay to avoid collision with schedule poll at 500ms)
        this->set_timeout("control_state_poll", 1000, [this]() {
          ESP_LOGD(TAG, "Polling control state to detect out-of-band changes (issue #54)");
          control_service_.get_mode_async([](bool success, ControlMode mode) {
            if (success) {
              ESP_LOGD(TAG, "Control state poll succeeded (mode=%d)", static_cast<uint8_t>(mode));
            } else {
              ESP_LOGW(TAG, "Control state poll failed");
            }
          });
        });
      }
    }

    // Check and perform daily time sync if needed
    check_and_sync_time();

    // Check for timed-out response handlers (2 second timeout)
    transport_.check_timeouts(2000);
  } else {
    ESP_LOGW(TAG, "Skipping polls - not ready");
  }
}

void AlphaHwrComponent::check_and_sync_time() {
  // Check if we need to sync (once per day)
  uint32_t now = millis();

  // Handle millis() rollover (every ~49 days)
  if (now < last_time_sync_timestamp_) {
    ESP_LOGD(TAG, "millis() rollover detected, resetting time sync tracking");
    last_time_sync_timestamp_ = 0;
  }

  // If never synced (0) or 24 hours have passed
  if (last_time_sync_timestamp_ == 0 ||
      (now - last_time_sync_timestamp_) >= TIME_SYNC_INTERVAL_MS) {
    // Check if system time is available via SNTP
    time_t current_time = ::time(nullptr);
    if (current_time < 1609459200) { // Before 2021-01-01 means time not synced
      ESP_LOGD(TAG,
               "System time not synced via SNTP yet, skipping pump clock sync");
      return;
    }

    ESP_LOGD(TAG, "Daily time sync due - syncing pump clock...");
    time_service_.set_clock_async([this](bool success) {
      if (success) {
        ESP_LOGD(TAG, "Daily pump clock sync successful");
        this->last_time_sync_timestamp_ = millis();

        // Update last sync time sensor
#ifdef USE_TEXT_SENSOR
        if (this->last_clock_sync_sensor_) {
          time_t now = ::time(nullptr);
          const struct tm *tm_info = localtime(&now);
          char buf[32];
          strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
          this->last_clock_sync_sensor_->publish_state(buf);
        }
#endif
      } else {
        ESP_LOGW(TAG, "Daily pump clock sync failed - will retry next update");
      }
    });
  }
}

void AlphaHwrComponent::read_device_info() {
  device_info_service_.read_device_info_async([this](bool success) {
    if (success) {
      ESP_LOGI(TAG, "Device info read completed successfully");
      // Publish device info strings to text sensors
#ifdef USE_TEXT_SENSOR
      if (this->product_name_sensor_) {
        this->product_name_sensor_->publish_state(
            device_info_service_.get_product_name());
      }
      if (this->serial_number_sensor_) {
        this->serial_number_sensor_->publish_state(
            device_info_service_.get_serial_number());
      }
      if (this->software_version_sensor_) {
        this->software_version_sensor_->publish_state(
            device_info_service_.get_software_version());
      }
      if (this->hardware_version_sensor_) {
        this->hardware_version_sensor_->publish_state(
            device_info_service_.get_hardware_version());
      }
      if (this->ble_version_sensor_) {
        this->ble_version_sensor_->publish_state(
            device_info_service_.get_ble_version());
      }
#endif
    } else {
      ESP_LOGW(TAG, "Device info read failed");
    }
  });
}

void AlphaHwrComponent::read_statistics() {
  device_info_service_.read_statistics_async(
      [this](bool success, uint32_t start_count, float operating_hours) {
        if (success) {
          ESP_LOGI(TAG, "Statistics read successful: %u starts, %.1f hours",
                   start_count, operating_hours);
          if (this->start_count_sensor_) {
            this->start_count_sensor_->publish_state(start_count);
          }
          if (this->operating_hours_sensor_) {
            this->operating_hours_sensor_->publish_state(operating_hours);
          }
        } else {
          ESP_LOGW(TAG, "Statistics read failed");
        }
      });
}

void AlphaHwrComponent::read_pump_clock() {
  ESP_LOGI(TAG, "Manual pump clock read requested");

  time_service_.get_clock_async([this](ESPTime pump_time) {
    if (pump_time.is_valid()) {
      time_t now = ::time(nullptr);
      double diff = difftime(pump_time.timestamp, now);

      ESP_LOGI(TAG, "Pump clock read successful: %04d-%02d-%02d %02d:%02d:%02d",
               pump_time.year, pump_time.month, pump_time.day_of_month,
               pump_time.hour, pump_time.minute, pump_time.second);
      ESP_LOGI(TAG, "Clock drift: %.0f seconds", diff);

      if (this->clock_diff_sensor_) {
        this->clock_diff_sensor_->publish_state(diff);
      }
    } else {
      ESP_LOGW(TAG, "Pump clock read failed or returned invalid time");
      if (this->clock_diff_sensor_) {
        this->clock_diff_sensor_->publish_state(NAN);
      }
    }
  });
}

void AlphaHwrComponent::authenticate() {
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }

  session_.on_authenticating();
  auth_.start();
}

void AlphaHwrComponent::gap_event_handler(esp_gap_ble_cb_event_t event,
                                          esp_ble_gap_cb_param_t *param) {
  // Delegate to BLE connection manager
  ble_manager_.handle_gap_event(event, param);
}

void AlphaHwrComponent::gattc_event_handler(esp_gattc_cb_event_t event,
                                            esp_gatt_if_t gattc_if,
                                            esp_ble_gattc_cb_param_t *param) {
  // Delegate to BLE connection manager
  ble_manager_.handle_gattc_event(event, gattc_if, param);
}

} // namespace alpha_hwr
} // namespace esphome
