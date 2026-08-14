#include "alpha_hwr.h"
#include "frame_parser.h"
#include "telemetry_decoder.h"
#include "esphome/core/application.h"  // App.get_build_time_string() for "Component Build"
#include "esphome/core/version.h"      // ESPHOME_VERSION_CODE / VERSION_CODE
#include <cinttypes>

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

  if (this->ready_sensor_) {
    this->ready_sensor_->publish_state(false);
  }

#ifdef USE_TEXT_SENSOR
  // "Component Build" (issue #124): which build of this component is running —
  // the source revision resolved by `git describe` at codegen, plus the
  // firmware build timestamp, which separates two flashes of the same revision.
  // The release version can't do this: it changes only at a release, so every
  // post-release build reports the same value and a behavior change cannot be
  // pinned to an install from Home Assistant history.
  if (this->component_build_sensor_ != nullptr) {
#if defined(ESPHOME_VERSION_CODE) && ESPHOME_VERSION_CODE >= VERSION_CODE(2026, 1, 0)
    char build_time[Application::BUILD_TIME_STR_SIZE];
    App.get_build_time_string(build_time);
#else
    // get_build_time_string() landed in 2026.1; older cores only have the
    // (now deprecated) std::string accessor.
    const std::string build_time_str = App.get_compilation_time();
    const char *build_time = build_time_str.c_str();
#endif
    char build_id[96];
    snprintf(build_id, sizeof(build_id), "%s (built %s)", this->build_revision_, build_time);
    this->component_build_sensor_->publish_state(build_id);
    ESP_LOGI(TAG, "Component build: %s", build_id);
  }
#endif

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
    // Start the inbound-data watchdog from the open, not from READY: the
    // subscribe paths that never call subscribed_callback_() leave the session
    // short of READY forever, and those need recycling too.
    this->link_last_inbound_ms_ = this->link_last_open_ms_;
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
    
    // Invalidate caches so old values don't falsely satisfy ready gating
    this->control_service_.invalidate_cache();
    this->schedule_service_.invalidate_cache();
    // Terminal-event every pending write operation (issue #92): a client
    // waiting on a settle event must never be left hanging across a BLE drop.
    this->write_op_service_.on_disconnect();
    
    if (this->ready_sensor_) {
      this->ready_sensor_->publish_state(false);
    }
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
      ESP_LOGI(TAG, "Disconnected; holding reconnect until pump reappears + %" PRIu32 " ms",
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
        // Inbound-data watchdog: stamped here, before any parsing, so that a
        // pump answering with frames this build cannot decode still counts as
        // alive. The watchdog is a liveness check on the link, not a
        // correctness check on the payload.
        this->link_last_inbound_ms_ = millis();

        // Pump Link Status: this — not auth completing — is what proves the
        // link works, so it is where a run of failed attempts is forgiven.
        // Gated so the common case is one branch, not two stores per frame.
        if (!this->link_reached_ready_) {
          this->link_reached_ready_ = true;
          this->link_consecutive_failures_ = 0;
        }
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

    // Pump Link Status: deliberately does NOT mark this a working link. Auth
    // completing proves only that a chain of timers ran; on a deaf link it
    // happens just the same. Clearing link_consecutive_failures_ here would
    // reset the count on every watchdog recycle, so a permanently deaf pump
    // could never accumulate the LINK_FAIL_K failures that surface the
    // "Reconnecting" rung — it would flip between Connected and Connecting
    // forever. The reset lives on the notification path instead, where inbound
    // data actually proves the link works.
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
      // The mode is re-read on every control-state poll, so this callback is
      // usually confirming what the sensor already holds. TextSensor has no
      // publish dedup, so republishing it costs an API frame per subscriber
      // every poll for nothing (issue #127).
      if (this->control_mode_sensor_->has_state() &&
          this->control_mode_sensor_->state == mode_name) {
        ESP_LOGV(TAG, "Control mode unchanged: %s", mode_name);
        return;
      }
      this->control_mode_sensor_->publish_state(mode_name);
      ESP_LOGI(TAG, "Published control mode to sensor: %s", mode_name);
    }
#endif
  });

  // Initialize the write-operation layer (issue #92)
  write_op_service_.set_schedule_callback(
      [this](std::function<void()> callback, uint32_t delay_ms) {
        this->set_timeout(delay_ms, std::move(callback));
      });
  write_op_service_.set_ready_check([this]() { return this->is_state_synchronized(); });

  // Central write-result hook (issue #92): refresh the schedule displays when
  // a schedule operation settles (the confirm readbacks have already updated
  // the caches), then forward every result to the API bridge, which fires the
  // esphome.alpha_hwr_write_settled event.
  write_op_service_.set_result_callback([this](const services::WriteResult &result) {
    using services::WriteCommand;
    using services::WriteStatus;
    bool applied = result.status == WriteStatus::ACCEPTED || result.status == WriteStatus::CLAMPED;

    // Schedule grid display (hash + per-layer sensors). The rule lives in
    // write_operation_service.h so the host test exercises the production
    // predicate; UPLOAD_SCHEDULE was missing from it entirely (issue #133),
    // which left sensor.<device>_schedule_hash on its old value after every
    // bulk upload. The sync model is "poll that sensor until it
    // matches", so a correctly-programmed pump read as a permanent sync
    // failure and the scheduler re-uploaded the same grid indefinitely.
    if (services::result_republishes_schedule(result))
      this->publish_schedule_hash();

    switch (result.command) {
      case WriteCommand::SET_SINGLE_EVENT:
      case WriteCommand::CLEAR_SINGLE_EVENT:
      case WriteCommand::REFRESH_SINGLE_EVENTS:
#ifdef USE_TEXT_SENSOR
        // Gated like the read path in the header: this fires on every
        // single-event write *and* every refresh_single_events settle, and the
        // string is usually identical (issue #127 / AGENTS §4).
        if (applied && this->single_events_text_sensor_ != nullptr) {
          publish_text_sensor_if_changed(
              this->single_events_text_sensor_,
              schedule_service_.format_single_events_display());
        }
        if (applied && this->vacation_text_sensor_ != nullptr) {
          publish_text_sensor_if_changed(
              this->vacation_text_sensor_,
              schedule_service_.format_vacation_display());
        }
#endif
        break;
      default:
        break;
    }
#ifdef ALPHA_HWR_HAS_API_BRIDGE
    api_bridge_.fire_write_settled(result);
#endif
  });

#ifdef ALPHA_HWR_HAS_API_BRIDGE
  // Register the programmatic services (issue #92).
  api_bridge_.setup(this);
#endif

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
      [this](bool enabled) { this->publish_schedule_hash(); });

  // Control mode text sensor will be populated when we receive the passive
  // notification from the pump during authentication. Do NOT publish a
  // default/unknown value here.
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
  ESP_LOGI(TAG, "Pump reappeared; holding reconnect %" PRIu32 " ms to let it settle",
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
    this->check_link_liveness_();
    this->evaluate_link_status();
  }
}

// Inbound-data watchdog. See link_watchdog.h for why a link can be open,
// authenticated and completely deaf, and why the remedy is a disconnect rather
// than a session-state transition.
void AlphaHwrComponent::check_link_liveness_() {
  if (!link_data_timeout_expired(this->session_.is_connected(), millis(),
                                 this->link_last_inbound_ms_, this->link_data_timeout_ms_)) {
    return;
  }

  ESP_LOGE(TAG, "No data from pump for %" PRIu32 " ms while %s - recycling the link",
           this->link_data_timeout_ms_, this->session_.get_state_name());

  // Count this as a failed attempt, even though the session may have reached
  // READY: by the watchdog's own evidence it never worked. Paired with the
  // notification path owning the reset (see the auth-completion callback), this
  // is what lets a persistently deaf pump accumulate LINK_FAIL_K failures and
  // surface as "Reconnecting" rather than flapping between Connected and
  // Connecting forever.
  this->link_reached_ready_ = false;

  // Whole seconds read better and cover every sane value, but the option takes
  // milliseconds: truncating would render 1500ms as "(1s)" and anything under a
  // second as "(0s)", i.e. a fault string that misreports its own trigger.
  char reason[64];
  if (this->link_data_timeout_ms_ % 1000 == 0) {
    snprintf(reason, sizeof(reason), "No data from pump (%" PRIu32 "s)",
             this->link_data_timeout_ms_ / 1000);
  } else {
    snprintf(reason, sizeof(reason), "No data from pump (%" PRIu32 "ms)",
             this->link_data_timeout_ms_);
  }
  // Re-arm before disconnecting. esp_ble_gattc_close() is asynchronous, so the
  // session stays is_connected() for some number of loop() ticks after this
  // call; without the re-arm the window is still expired on every one of them
  // and the watchdog would re-fire (and re-latch its failure reason) each tick
  // until the DISCONNECT event finally lands.
  this->link_last_inbound_ms_ = millis();
  this->ble_manager_.force_disconnect(reason);
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

// True when an enabled Stop single-event (a vacation) covers *now*. During one
// the pump is commanded to stay stopped, so STOP + schedule-on is expected, not
// a dead schedule (issue #124). Single-event timestamps are UTC epoch seconds.
// Unknown time or an uncached single-event list answers false: the dead-schedule
// repair matters more than a hypothetical vacation we cannot confirm, and the
// repair is capped at one attempt per connection either way.
bool AlphaHwrComponent::stop_single_event_active_() const {
  if (!schedule_service_.is_single_events_cached()) return false;
  time_t now = ::time(nullptr);
#ifdef USE_TIME
  if (this->time_id_ != nullptr) now = this->time_id_->now().timestamp;
#endif
  if (now < 1609459200) return false;  // System clock not synced yet
  const uint32_t now_ts = static_cast<uint32_t>(now);
  for (const auto &ev : schedule_service_.get_cached_single_events()) {
    if (!ev.enabled || ev.action != 0x01) continue;  // 0x01 = Stop (vacation)
    if (ev.begin_timestamp <= now_ts && now_ts < ev.end_timestamp) return true;
  }
  return false;
}

// Run-state reconciliation (issue #124). Two jobs, both driven off the polled
// caches so they also catch out-of-band changes (issue #54's 30s control poll):
//
//   1. Publish the run state (off/engaged/scheduled/stalled) and the "schedule
//      stalled" problem flag. Needed because "Engage Pump" reads AUTO &&
//      !schedule, so with the schedule on it shows off for BOTH AUTO and STOP —
//      a dead schedule was indistinguishable from a healthy scheduled pump.
//   2. Repair the dead schedule. STOP + schedule-on can never run a window, and
//      apply_pump_schedule_target_() is only reachable from command paths, so
//      before this the state survived every reboot. Converging it to Scheduled
//      keeps the schedule intent and engages AUTO so windows run again.
//
// The repair only runs with valid caches and an idle write queue, and attempts
// are throttled to one per DEAD_SCHEDULE_REPAIR_MIN_INTERVAL_MS, so it cannot
// fight a user command or spin against a pump that keeps reverting. It reports
// itself as WriteOrigin::INTERNAL with op_id "auto:dead-schedule-repair" so a
// write_settled consumer can tell a self-repair from a user action.
void AlphaHwrComponent::reconcile_run_state_() {
  bool schedule_on = false;
  if (!control_service_.is_pump_enabled_valid() ||
      !schedule_service_.get_state(&schedule_on)) {
    return;  // Not cached yet — nothing honest to publish or repair.
  }
  const bool pump_auto = control_service_.is_pump_enabled();
  const bool stop_event = this->stop_single_event_active_();
  const bool stalled = ux::is_dead_schedule(pump_auto, schedule_on, stop_event);

  const char *run_state = ux::run_state_display(pump_auto, schedule_on, stop_event);
  if (run_state_published_ == nullptr || std::strcmp(run_state_published_, run_state) != 0) {
    run_state_published_ = run_state;
#ifdef USE_TEXT_SENSOR
    if (this->pump_run_state_sensor_ != nullptr) {
      this->pump_run_state_sensor_->publish_state(run_state);
    }
#endif
    ESP_LOGD(TAG, "Pump run state: %s", run_state);
  }
  if (this->schedule_stalled_sensor_ != nullptr &&
      this->stalled_published_ != static_cast<int8_t>(stalled)) {
    this->stalled_published_ = static_cast<int8_t>(stalled);
    this->schedule_stalled_sensor_->publish_state(stalled);
  }

  if (!stalled || write_op_service_.pending_count() > 0) return;

  // Throttle: unsigned subtraction is rollover-correct.
  const uint32_t now = millis();
  if (this->repair_attempted_ &&
      (now - this->last_repair_attempt_ms_) < DEAD_SCHEDULE_REPAIR_MIN_INTERVAL_MS) {
    return;
  }
  this->repair_attempted_ = true;
  this->last_repair_attempt_ms_ = now;
  ESP_LOGW(TAG,
           "Schedule is enabled but the pump is STOP - no window can run it. "
           "Engaging AUTO to repair (issue #124).");
  this->apply_pump_schedule_target_(ux::dead_schedule_repair_target(),
                                    services::WriteOrigin::INTERNAL,
                                    "auto:dead-schedule-repair");
}

bool AlphaHwrComponent::is_state_synchronized() const {
  if (!session_.is_ready() || !initial_data_read_done_) return false;
  return control_service_.is_cache_valid() && schedule_service_.is_overview_cache_valid();
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
#ifdef USE_TIME
        if (this->time_id_) now = this->time_id_->now().timestamp;
#endif
        if (now > 1609459200) {
          // Calculate drift (Pump Time - System Time)
          // If pump is ahead, diff is positive. If pump is behind, diff is
          // negative.
          double diff = difftime(pump_time.timestamp, now);

          ESP_LOGI(TAG, "Pump clock drift before sync: %.0f seconds", diff);

          if (this->clock_diff_sensor_) {
            this->clock_diff_sensor_->publish_state(diff);
          }
        } else {
          ESP_LOGI(TAG, "System time not synced yet; skipping drift calculation");
        }
      } else {
        ESP_LOGW(TAG, "Could not read pump clock for drift measurement");
        // Publish NAN to indicate invalid/unknown drift
        if (this->clock_diff_sensor_) {
          this->clock_diff_sensor_->publish_state(NAN);
        }
      }

      // Now sync the clock
      bool can_sync = false;
#ifdef USE_TIME
      can_sync = (this->time_id_ != nullptr);
#endif
      if (!can_sync) {
        ESP_LOGD(TAG, "Skipping pump clock sync (no time_id configured)");
        return;
      }

      this->time_service_.set_clock_async([this](bool success) {
        if (success) {
          ESP_LOGD(TAG, "Initial pump clock sync successful");
          this->last_time_sync_timestamp_ = millis();

          // Update last sync time sensor
#ifdef USE_TEXT_SENSOR
          if (this->last_clock_sync_sensor_) {
            char buf[32];
            bool used_time_id = false;
#ifdef USE_TIME
            if (this->time_id_) {
              this->time_id_->now().strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S");
              used_time_id = true;
            }
#endif
            if (!used_time_id) {
              time_t now = ::time(nullptr);
              const struct tm *tm_info = localtime(&now);
              strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
            }
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

  // Query control mode, setpoints, config, and cycle time bounds
  this->set_timeout(5000, [this, gen]() {
    this->do_control_cache_sync(gen);
  });
}

void AlphaHwrComponent::do_control_cache_sync(uint32_t gen) {
  if (gen != this->read_chain_gen_) return;
  ESP_LOGD(TAG, "Syncing full control cache (Mode, Setpoints, Config)...");
  this->control_service_.sync_cache_async([this, gen](bool success) {
    if (gen != this->read_chain_gen_) return;
    if (success) {
      ESP_LOGD(TAG, "Control cache sync complete");
    } else {
      ESP_LOGW(TAG, "Control cache sync failed, retrying in 5s");
      this->set_timeout(5000, [this, gen]() {
        this->do_control_cache_sync(gen);
      });
    }
  });
}

// Called every 10 seconds by PollingComponent
void AlphaHwrComponent::update() {
  // The sentinel is 0xFFFF (esp32_ble_client::UNSET_CONN_ID) on a uint16_t
  // conn_id, so the 0xFF this used to compare against was never equal to it and
  // the guard could not fire: every poll ran regardless of whether a connection
  // handle existed. Named constant rather than a literal so it cannot drift
  // out of step with the base class again.
  if (session_.is_ready() && parent_ &&
      parent_->get_conn_id() != esp32_ble_client::UNSET_CONN_ID) {
    // If session is ready but initial data reads haven't been triggered yet
    // (e.g., BLE connection persisted through ESP32 restart, no re-auth),
    // trigger them now.
    if (!initial_data_read_done_) {
      ESP_LOGD(TAG,
               "Session ready but initial data not yet read - triggering now");
      telemetry_service_.start();
      trigger_initial_data_reads();
    } else {
      // If we are initialized, evaluate cache validity to update Ready sensor
      if (ready_sensor_ && !ready_sensor_->state && is_state_synchronized()) {
        ESP_LOGI(TAG, "Cache synchronized: pump is fully READY for control");
        ready_sensor_->publish_state(true);
      }

      // Publish the run state and repair a dead schedule (issue #124). Gated on
      // full sync so it never acts on a half-populated cache; this is also the
      // only path that reconciles run state at boot.
      if (is_state_synchronized()) {
        reconcile_run_state_();
      }
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
    // app control). Scheduled via set_timeout() with 1000ms delay after telemetry
    // polls to minimize BLE traffic collisions.
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
          control_service_.get_mode_async([](bool success, services::ControlMode mode) {
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
#ifdef USE_TIME
    if (this->time_id_) current_time = this->time_id_->now().timestamp;
#endif
    if (current_time < 1609459200) { // Before 2021-01-01 means time not synced
      ESP_LOGD(TAG,
               "System time not synced via SNTP yet, skipping pump clock sync");
      return;
    }

    bool can_sync = false;
#ifdef USE_TIME
    can_sync = (this->time_id_ != nullptr);
#endif
    if (!can_sync) {
      ESP_LOGD(TAG, "Skipping pump clock sync (no time_id configured)");
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
          char buf[32];
          bool used_time_id = false;
#ifdef USE_TIME
          if (this->time_id_) {
            this->time_id_->now().strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S");
            used_time_id = true;
          }
#endif
          if (!used_time_id) {
            time_t now = ::time(nullptr);
            const struct tm *tm_info = localtime(&now);
            strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info);
          }
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
          ESP_LOGI(TAG, "Statistics read successful: %" PRIu32 " starts, %.1f hours",
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
#ifdef USE_TIME
      if (this->time_id_) now = this->time_id_->now().timestamp;
#endif
      if (now > 1609459200) {
        double diff = difftime(pump_time.timestamp, now);

        ESP_LOGI(TAG, "Pump clock read successful: %04d-%02d-%02d %02d:%02d:%02d",
                 pump_time.year, pump_time.month, pump_time.day_of_month,
                 pump_time.hour, pump_time.minute, pump_time.second);
        ESP_LOGI(TAG, "Clock drift: %.0f seconds", diff);

        if (this->clock_diff_sensor_) {
          this->clock_diff_sensor_->publish_state(diff);
        }
      } else {
        ESP_LOGW(TAG, "System time not synced yet; cannot calculate drift");
        if (this->clock_diff_sensor_) {
          this->clock_diff_sensor_->publish_state(NAN);
        }
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
