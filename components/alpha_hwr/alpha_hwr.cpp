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

  // `time_id` is optional in the schema and load-bearing in practice, and its
  // absence used to be reported only at DEBUG -- invisible at the INFO level
  // this component ships. What the user sees instead is a pump whose schedule
  // windows quietly drift, because the pump runs them off its own RTC and
  // nothing is ever correcting it. Say so once, where it can be read.
  if (!this->has_wall_clock_()) {
    ESP_LOGW(TAG, "No time_id configured - the pump clock will never be synced");
    ESP_LOGW(TAG, "  The pump runs schedule windows off its own RTC, which drifts");
    ESP_LOGW(TAG, "  Add `time_id:` pointing at a time component to enable syncing");
    // Deliberately does not arm the repeat throttle. setup() runs at
    // setup_priority::DATA, well before the API server is up and long before any
    // network log client can attach, so this pass reaches the serial console
    // only. The repeat from check_and_sync_time() is what an `esphome logs`
    // reader actually sees.
  }

  // A gap histogram configured under a budget that cannot let its top rungs
  // fill (issue #176 part 1). The watchdog closes an interval on the first tick
  // past the window in force, so every threshold at or above `data_timeout`
  // reads a structural zero -- which is indistinguishable from "the pump never
  // went quiet that long", and is exactly the wrong conclusion. Worth a warning
  // rather than silence: the failure mode is a measurement run that looks
  // clean, produces reassuring numbers, and settles the default on nothing.
  const uint32_t top_rung_ms = this->link_gap_top_configured_rung_ms_();
  if (link_gap_thresholds_censored(this->link_data_timeout_ms_, top_rung_ms)) {
    ESP_LOGW(TAG,
             "Link gap histogram counts up to %" PRIu32
             "s, but data_timeout is %" PRIu32 "s",
             top_rung_ms / 1000, this->link_data_timeout_ms_ / 1000);
    // Above the budget and equal to it fail differently, and saying "cannot
    // fill" for both is wrong about the second: a rung at the budget does
    // increment, on the samples the watchdog itself cut off. It stops being a
    // gap count and becomes a recycle count, which is the more insidious of the
    // two because the number looks alive.
    if (top_rung_ms == this->link_data_timeout_ms_) {
      ESP_LOGW(TAG, "  That counter can only be reached by intervals the "
                    "watchdog cut off - it counts recycles, not quiet periods");
    } else {
      ESP_LOGW(TAG, "  Counters above that budget cannot fill - the watchdog "
                    "truncates the interval first");
    }
    ESP_LOGW(TAG, "  Raise data_timeout (600s) for a measurement run; see "
                  "docs/configuration.md");
  }

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
    // Arm the readiness watchdog from the same stamp -- and from here ONLY.
    // Every other candidate site (a notification, a session transition, a cache
    // filling) is something that happens on the way to readiness, so re-arming
    // there would make the timer chase the thing it is waiting for and never
    // expire. See readiness_watchdog.h (issue #211).
    this->link_ready_since_ms_ = this->link_last_open_ms_;
    this->link_pump_ready_seen_ = false;
    // ...and start the gap sampler from the same stamp, so what it reports is
    // what the watchdog acts on. The interval that ends here is not sampled:
    // the link was down for it, and the watchdog does not run on a down link.
    this->link_gap_.on_open(this->link_last_open_ms_);
    this->evaluate_link_status();
  });

  ble_manager_.set_disconnection_callback([this]() {
    // Close the gap interval this drop ended. The watchdog was timing it
    // against its budget until the link went away, so leaving it unrecorded
    // censors the sample the same way dropping the recycle sample did -- a link
    // that goes quiet and then drops on its own (supervision timeout, pump
    // power loss, the encryption-failure teardown) would report only its
    // steady-state cadence. The sampler ignores this if no open preceded it.
    this->link_gap_.on_disconnect(millis());
    // Cancel the pending stabilize timer, so a disconnect inside that window
    // cannot leave it to declare the NEXT connection ready before it has
    // stabilized (issue #15).
    this->cancel_timeout(SESSION_READY_TIMER);
    // Stop telemetry so the next ready callback can restart it cleanly.
    this->telemetry_service_.stop();
    // Reset initial-read flag so device info, clock sync, etc. are re-fetched
    // after reconnect (pump may have rebooted).
    this->initial_data_read_done_ = false;
    // The chain re-runs on reconnect, so its products must be re-proven and
    // the backoff must start over rather than inheriting the old link's.
    this->device_info_read_ok_ = false;
    this->statistics_read_ok_ = false;
    this->initial_clock_sync_started_ = false;
    this->initial_read_retry_interval_ms_ = INITIAL_READ_SYNC_TIMEOUT_MS;
    
    // Invalidate caches so old values don't falsely satisfy ready gating
    this->control_service_.invalidate_cache();
    this->schedule_service_.invalidate_cache();
    // Terminal-event every pending write operation (issue #92): a client
    // waiting on a settle event must never be left hanging across a BLE drop.
    this->write_op_service_.on_disconnect();
    
    if (this->ready_sensor_) {
      this->ready_sensor_->publish_state(false);
    }
    // Clear the readiness latch HERE, alongside the sensor it mirrors, and not
    // only at connection-open. Clearing it only on open left it true for the
    // whole disconnected window -- and the fault surface reads it as "healthy",
    // so Pump Link Fault published "None" across exactly the reconnect that
    // docs/configuration.md promises will show "No data from pump (60s)". That
    // regression reached every user regardless of ready_timeout, because the
    // display gate is not conditional on the watchdog being enabled.
    //
    // The rule is simply that this tracks the same thing the sensor does: the
    // pump is usable, and a link that is down is not usable. The clear at
    // connection-open stays as well; it is what arms the watchdog for the new
    // connection.
    this->link_pump_ready_seen_ = false;
    // Invalidate pending initial-read-chain timers so a disconnect mid-chain
    // can't leave them to fire reads against the next connection (issue #18).
    // The chain re-runs fresh on reconnect, so nothing is lost.
    this->read_chain_gen_++;
    this->session_.on_disconnected();
    // Fails every queued command -- each callback is invoked with failure, the
    // same shape a timeout reports (issue #259) -- and clears the pending
    // handlers, the reassembly buffer and the FSM state.
    //
    // Everything above this line runs FIRST on purpose, and that order is
    // load-bearing rather than incidental: the callbacks invoked here are
    // service code, and they are inert only because the caches are already
    // invalidated, every write operation is already terminal, and the read-chain
    // generation has already moved. Moving reset() earlier would run them
    // against a component that still believes the link is up.
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
    // ...and not while suspended (issue #243). The settle path exists to hold a
    // reconnect off briefly; arming it during a suspend schedules the timer that
    // hands auto-connect straight back, so the switch would release itself a
    // couple of seconds after being flipped. Reported from a node running the
    // default settle window: suspended for 2.3 seconds, all of it the window.
    //
    // Belt-and-braces rather than the fix, and worth being honest about which.
    // The guard on the timer callback below is what actually holds the
    // suspension -- delete THIS term and the suspension still survives, because
    // the timer fires into a suspended link and declines to act. Verified by
    // removing each in turn. What this one buys is that the component does not
    // arm a state machine it has no use for during a suspend, and does not log
    // "holding reconnect until pump reappears" about a link nobody is waiting
    // to reconnect.
    if (!this->suspended_ && this->reconnect_settle_ms_ > 0 &&
        this->parent_ != nullptr && esp_ble_get_bond_device_num() > 0) {
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
    this->session_.on_subscribed();  // -> STABILIZING

    // Nothing is sent between the CCCD write and the initial read chain.
    //
    // What used to go here were four GENIbus reads -- a Class 2 GET of
    // unit_family/unit_type/unit_version, a Class 10 GET of the operation-status
    // object, and two INFO queries -- called an authentication handshake and
    // documented as unlocking control. They are reads, so they cannot change
    // device state, and every one of their replies was discarded. A pump
    // reached Pump Ready across ten connection cycles without them, including
    // two bond-cleared re-pairings and five power cycles, reading all five
    // Class 7 strings and accepting Class 3 START and STOP each time
    // (issue #174). So the sequence is gone, and what is left of this step is
    // the delay.
    //
    // The delay stays, because it was never part of the sequence. It separates
    // the CCCD write and the encryption negotiation behind it from the first
    // GENI traffic (issues #12/#13), and each connection already takes a run at
    // the window where an encryption request can fail with 0x61 and erase the
    // bond (issue #14). Removing the reads moves first traffic ~1.3 s earlier
    // on its own; taking this out too would compound that with nothing measured
    // behind it. See SESSION_STABILIZE_MS.
    this->set_timeout(SESSION_READY_TIMER, SESSION_STABILIZE_MS,
                      [this]() { this->on_session_stabilized_(); });
  });

  ble_manager_.set_notification_callback(
      [this](const uint8_t *data, size_t len) {
        // Inbound-data watchdog: stamped here, before any parsing, so that a
        // pump answering with frames this build cannot decode still counts as
        // alive. The watchdog is a liveness check on the link, not a
        // correctness check on the payload.
        const uint32_t inbound_now = millis();

        // Close the quiet interval this frame ended (issue #176). The
        // data_timeout default has to clear every gap that happens routinely
        // while staying short enough to be useful, and those two requirements
        // pull opposite ways -- only observation settles it. Recorded here
        // rather than derived from the poll interval because a bench reading
        // once turned out to be 90 s old against a channel the constants said
        // was 10 s.
        //
        // This feeds the whole distribution, not just the running maximum: the
        // per-threshold counters, how many intervals were cut short rather than
        // ending on their own, and the time they cover. See LinkGapSampler.
        this->link_gap_.on_inbound(inbound_now);

        // Data arrived, so whatever the link was doing, it is doing it again --
        // but only frames received while the session is READY count as that
        // proof. Nothing is sent between the CCCD write and READY any more
        // (issue #174), so a pre-READY frame can only be one the pump
        // volunteered -- and link_watchdog.h records that this specimen does
        // volunteer operation-status notifications. A pump that emits one and
        // then goes silent delivers one frame per session. Resetting on those
        // would clear the window and the
        // counter once per session forever: the backoff would never engage, and
        // the counter an automation is supposed to threshold on would read 0
        // while the node recycled ~1,200 times a day. Simulated at exactly that.
        //
        // Post-READY frames are the poll responses, which are what "the link
        // works" actually means. A pump that comes back after hours reaches
        // READY and resets from there.
        if (this->session_.is_ready()) {
          this->link_data_timeout_current_ms_ = this->link_data_timeout_ms_;
          this->link_recycles_without_data_ = 0;
        }

        this->link_last_inbound_ms_ = inbound_now;

        // Pump Link Status: this — not the session being declared ready — is
        // what proves the link works, so it is where a run of failed attempts
        // is forgiven.
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
    (void) operation_mode;
    (void) setpoint;
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

  schedule_service_.set_state_change_callback(
      [this](bool enabled) {
        (void) enabled;  // the hash is recomputed from the cache, not the flag
        this->publish_schedule_hash();
      });

  // Control mode text sensor will be populated from the operation-status
  // notification -- volunteered by the pump, or fetched by the control-state
  // sync on the initial read chain. Do NOT publish a default/unknown value
  // here.
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
    // Guard 1 stops this timer being armed during a suspend, but not a suspend
    // that arrives while it is already running. Restoring auto-connect here
    // would silently undo the switch (issue #243).
    if (this->parent_ != nullptr && !this->suspended_) {
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
    // Liveness first, and only one teardown per tick. Both windows can be
    // expired at once -- a configuration with data_timeout above ready_timeout
    // reaches that on an ordinary silent link -- and running both fired
    // client_->disconnect() twice in one tick and let the second reason
    // overwrite the first. Silence is the more specific diagnosis of the two,
    // so it goes first and, having acted, is left to stand.
    if (!this->check_link_liveness_())
      this->check_link_readiness_();
    this->evaluate_link_status();
    // The tick's own stamp, not a fresh millis(): the watched-time throttle
    // measures against it, and re-reading the clock after two calls that can
    // each take a while would make the interval it enforces drift.
    this->publish_link_diagnostics_(this->link_last_eval_ms_);
  }
}

// Inbound-data watchdog. See link_watchdog.h for why a link can be open,
// ready and completely deaf, and why the remedy is a disconnect rather
// than a session-state transition.
bool AlphaHwrComponent::check_link_liveness_() {
  if (!link_data_timeout_expired(this->session_.is_connected(), millis(),
                                 this->link_last_inbound_ms_,
                                 this->link_data_timeout_current_ms_)) {
    return false;
  }

  // The window that actually expired, kept for the log and the fault string
  // before the backoff below moves it. Reporting the configured value here
  // would misstate the trigger once the window has grown.
  const uint32_t expired_ms = this->link_data_timeout_current_ms_;

  // Consecutive recycles that produced no data (issue #176). Reset only by a
  // notification received while the session is READY (see the reset site in the
  // notification callback: a frame the pump volunteered before READY does not
  // count as proof the link works), so it reads 0 in normal operation and an
  // automation can threshold
  // on it -- a value to alert on rather than a flap cadence somebody has to be
  // watching to notice.
  this->link_recycles_without_data_++;

  // Widen the next window. See link_data_timeout_next(): a recoverable link
  // still recovers on the first or second try, while a permanently deaf one
  // drops from ~1,300 recycles a day to about 28, bounding the number of runs
  // at the encryption-on-open window that can erase the bond (issue #14).
  this->link_data_timeout_current_ms_ =
      link_data_timeout_next(this->link_data_timeout_current_ms_,
                             LINK_DATA_TIMEOUT_BACKOFF_CAP_MS);

  ESP_LOGE(TAG,
           "No data from pump for %" PRIu32 " ms while %s - recycling the link "
           "(consecutive: %" PRIu32 ", next window %" PRIu32 " ms)",
           expired_ms, this->session_.get_state_name(),
           this->link_recycles_without_data_,
           this->link_data_timeout_current_ms_);

  // Count this as a failed attempt, even though the session may have reached
  // READY: by the watchdog's own evidence it never worked. Paired with the
  // notification path owning the reset (see on_session_stabilized_()), this
  // is what lets a persistently deaf pump accumulate LINK_FAIL_K failures and
  // surface as "Reconnecting" rather than flapping between Connected and
  // Connecting forever.
  this->link_reached_ready_ = false;

  // Whole seconds read better and cover every sane value, but the option takes
  // milliseconds: truncating would render 1500ms as "(1s)" and anything under a
  // second as "(0s)", i.e. a fault string that misreports its own trigger.
  char reason[64];
  if (expired_ms % 1000 == 0) {
    snprintf(reason, sizeof(reason), "No data from pump (%" PRIu32 "s)",
             expired_ms / 1000);
  } else {
    snprintf(reason, sizeof(reason), "No data from pump (%" PRIu32 "ms)",
             expired_ms);
  }
  // Re-arm before disconnecting. esp_ble_gattc_close() is asynchronous, so the
  // session stays is_connected() for some number of loop() ticks after this
  // call; without the re-arm the window is still expired on every one of them
  // and the watchdog would re-fire (and re-latch its failure reason) each tick
  // until the DISCONNECT event finally lands.
  const uint32_t rearm_ms = millis();
  // Record the interval being given up on before the re-arm erases it. This is
  // the one sample the gap statistic cannot afford to drop: it is the only kind
  // that reaches the budget, and dropping it censors the sample at exactly the
  // threshold the statistic exists to validate (see LinkGapSampler).
  this->link_gap_.on_recycle(rearm_ms);
  this->link_last_inbound_ms_ = rearm_ms;
  // The readiness window is re-armed here too, and skipping it left the guard
  // in loop() a half fix. That guard stops both watchdogs firing in the SAME
  // tick; without this the readiness window is still expired on the NEXT one,
  // so a silent link produced two teardowns a second apart and counted two
  // recycles for one outage -- on the very counter this change added because
  // the reporter of issue #211 watches it. A link the liveness watchdog just
  // tore down has not had its readiness window fairly consumed either.
  this->link_ready_since_ms_ = rearm_ms;
  this->ble_manager_.force_disconnect(reason);
  return true;
}

// Readiness watchdog. See readiness_watchdog.h for why the inbound-data
// watchdog cannot cover this: the pump volunteers telemetry, so a session stuck
// anywhere at all keeps re-arming that one while never becoming usable.
void AlphaHwrComponent::check_link_readiness_() {
  if (!link_readiness_timeout_expired(this->session_.is_connected(),
                                      this->link_pump_ready_seen_, millis(),
                                      this->link_ready_since_ms_,
                                      this->link_ready_timeout_current_ms_)) {
    return;
  }

  const uint32_t expired_ms = this->link_ready_timeout_current_ms_;
  // Widen and re-arm before deciding what to DO about it. Both branches below
  // need this: the recycling one so the asynchronous close cannot re-fire it on
  // every tick, and the naming one because there is no teardown to interrupt
  // the condition at all -- without a re-arm it would re-latch every second
  // forever. Backed off, it reports on an escalating cadence instead, and the
  // string carries the window it actually waited.
  this->link_ready_timeout_current_ms_ =
      link_readiness_timeout_next(this->link_ready_timeout_current_ms_,
                                  LINK_READY_TIMEOUT_BACKOFF_CAP_MS);

  char reason[64];
  if (expired_ms % 1000 == 0) {
    snprintf(reason, sizeof(reason), "Pump never became ready (%" PRIu32 "s)",
             expired_ms / 1000);
  } else {
    snprintf(reason, sizeof(reason), "Pump never became ready (%" PRIu32 "ms)",
             expired_ms);
  }

  // Re-arm before anything else. On the recycling branch this is because
  // esp_ble_gattc_close() is asynchronous and the session stays connected for
  // some ticks; on the naming branch it is because nothing interrupts the
  // condition at all.
  const uint32_t rearm_ms = millis();
  this->link_ready_since_ms_ = rearm_ms;

  if (!this->link_ready_recycle_) {
    // Naming without recycling: the default, and the half of this feature that
    // carries no hazard. It is also the half the reporter of issue #211 said he
    // could not build from outside -- an automation can already see that Pump
    // Ready has been off for a while, but cannot tell *starting up* from
    // *stuck*, and only the component can name that.
    //
    // Deliberately does NOT touch link_recycles_without_ready_: nothing was
    // recycled, and counting one on a surface called "recycles" would be a lie
    // an automation thresholds on.
    ESP_LOGW(TAG,
             "Pump connected %" PRIu32 " ms without becoming ready while %s "
             "(next report in %" PRIu32 " ms; set ready_recycle to reconnect)",
             expired_ms, this->session_.get_state_name(),
             this->link_ready_timeout_current_ms_);
    this->ble_manager_.note_failure(reason, core::FailureHold::READY);
    return;
  }

  this->link_recycles_without_ready_++;

  ESP_LOGE(TAG,
           "Pump connected %" PRIu32 " ms without becoming ready while %s - "
           "recycling the link (consecutive: %" PRIu32 ", next window %" PRIu32
           " ms)",
           expired_ms, this->session_.get_state_name(),
           this->link_recycles_without_ready_,
           this->link_ready_timeout_current_ms_);

  // Same reasoning as the liveness watchdog: by this watchdog's own evidence
  // the connection never worked, whatever the session state says.
  this->link_reached_ready_ = false;
  // The data window is consumed by this outage too; without it the liveness
  // watchdog fires seconds later and counts the same outage twice.
  this->link_last_inbound_ms_ = rearm_ms;
  this->ble_manager_.force_disconnect(reason, core::FailureHold::READY);
}


// Link diagnostics for issue #176: the consecutive-recycle counter an
// automation can threshold on, and the longest quiet interval seen, which is
// what a data-driven data_timeout default has to be chosen from.
//
// Gated on change. sensor::Sensor::publish_state() does not dedup, so
// republishing on every ~1 s tick would cost a frame per API subscriber per
// second for values that change at most once per recycle -- the exact shape of
// the load that OOMs this node (issue #127).
void AlphaHwrComponent::publish_link_diagnostics_(uint32_t now_ms) {
  if (this->link_recycles_sensor_ != nullptr &&
      this->link_recycles_published_ !=
          this->link_recycles_without_data_ + this->link_recycles_without_ready_) {
    this->link_recycles_published_ =
        this->link_recycles_without_data_ + this->link_recycles_without_ready_;
    this->link_recycles_sensor_->publish_state(
        static_cast<float>(this->link_recycles_without_data_ +
                           this->link_recycles_without_ready_));
  }

  const uint32_t max_gap_ms = this->link_gap_.max_ms();
  if (this->link_max_gap_sensor_ != nullptr &&
      this->link_max_gap_published_ != max_gap_ms) {
    this->link_max_gap_published_ = max_gap_ms;
    this->link_max_gap_sensor_->publish_state(static_cast<float>(max_gap_ms) /
                                              1000.0f);
  }

  // The tail histogram (issue #176 part 1). Each rung is the number of times a
  // data_timeout of that length would have fired, so these are the counts the
  // default has to be chosen from; link_max_gap above gives one point of the
  // same distribution and cannot give its shape.
  for (size_t i = 0; i < LINK_GAP_BUCKETS; i++) {
    const uint32_t count = this->link_gap_.over_count(i);
    if (this->link_gap_over_sensors_[i] != nullptr &&
        this->link_gap_over_published_[i] != count) {
      this->link_gap_over_published_[i] = count;
      this->link_gap_over_sensors_[i]->publish_state(static_cast<float>(count));
    }
  }

  const uint32_t truncated = this->link_gap_.truncated();
  if (this->link_gaps_truncated_sensor_ != nullptr &&
      this->link_gaps_truncated_published_ != truncated) {
    this->link_gaps_truncated_published_ = truncated;
    this->link_gaps_truncated_sensor_->publish_state(
        static_cast<float>(truncated));
  }

  // Watched time: change-gated AND throttled. It advances on every notification,
  // so the gate alone would emit a frame per API subscriber every 10 s forever
  // -- the load shape that OOMs this node (issue #127). The throttle costs no
  // information: 300 s is Home Assistant's short-term statistics bucket and its
  // long-term statistics are hourly, so nothing downstream can resolve finer.
  //
  // The first publish is exempt from the throttle, not from the gate: the zero
  // baseline has to reach Home Assistant promptly at boot for its
  // total_increasing accounting to attribute the counts to the right run.
  const uint32_t watch_time_s =
      static_cast<uint32_t>(this->link_gap_.watched_ms() / 1000u);
  const bool watch_time_first =
      this->link_watch_time_published_ == 0xFFFFFFFFu;
  if (this->link_watch_time_sensor_ != nullptr &&
      this->link_watch_time_published_ != watch_time_s &&
      (watch_time_first || now_ms - this->link_watch_time_publish_ms_ >=
                               LINK_GAP_WATCH_PUBLISH_MS)) {
    this->link_watch_time_published_ = watch_time_s;
    this->link_watch_time_publish_ms_ = now_ms;
    this->link_watch_time_sensor_->publish_state(
        static_cast<float>(watch_time_s));
  }
}

// Pump Link Status state machine. The status is the FIRST matching condition
// below (a priority ladder), re-evaluated on the connection/disconnection/ready
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
// Diagnostic suspend (issue #243). Two calls and three guards; the guards are
// the whole of it, because the component otherwise fights its own switch.
void AlphaHwrComponent::set_suspended(bool suspended) {
  if (suspended == this->suspended_) return;
  this->suspended_ = suspended;

  if (suspended) {
    // The mask goes up FIRST, so the disconnect below cannot publish a fault
    // before evaluate_link_status() next runs.
    this->suspend_fault_mask_ = true;
    if (this->parent_ != nullptr) {
      // Without this the client reconnects from IDLE on the next matching
      // advertisement -- `ble_client.disconnect` alone is not a suspend, which
      // is why powering the node down was the only thing that worked.
      this->parent_->set_auto_connect(false);
    }
    this->ble_manager_.suspend_link();
  } else {
    ESP_LOGI(TAG, "Releasing BLE suspend; reconnecting to the pump");
    if (this->parent_ != nullptr) {
      this->parent_->set_auto_connect(true);
    }
    // suspend_fault_mask_ deliberately stays up; see its declaration. It comes
    // down when the pump is READY again, alongside link_pump_ready_seen_.
  }
  this->evaluate_link_status();
}

void AlphaHwrComponent::evaluate_link_status() {
#ifdef USE_TEXT_SENSOR
  // Companion sensor: show the latched failure reason only while the link is
  // unhealthy; read "None" once it is working again, so a stale reason does not
  // sit next to a healthy status. "None" is also the pre-failure default.
  //
  // "Healthy" is the pump being READY, not the SESSION being ready, and the
  // difference is the whole of issue #211. The session reaches ready a fixed
  // two seconds after subscribe with no frame exchanged; the pump is usable
  // only once the initial reads have landed and the caches are valid. Gating on
  // the session meant that a link stuck in exactly the reported state -- session
  // ready, telemetry flowing, Pump Ready off forever -- would latch the
  // readiness fault and then blank it, publishing "None" over the one string
  // that explained what was wrong. The diagnosis would have existed and been
  // unreadable.
  //
  // The cost is that a stale reason from the previous connection is now visible
  // for the 13-18 s between session-ready and pump-ready rather than being
  // hidden at the two-second mark. That is the honest reading: the link is not
  // usable yet during that window.
  if (this->pump_last_link_failure_sensor_ != nullptr) {
    const std::string &lf = this->ble_manager_.get_last_failure();
    // Three statements rather than one `||` chain, because a line containing a
    // `|` cannot be anchored by tools/mutation_check.sh -- its entries are split
    // on that character, so the search field is truncated mid-expression. The
    // one-liner was written first and --verify refused it. Restructuring for the
    // tool is the right trade: an entry that cannot be written is a hole that
    // reads as covered.
    bool show_none = this->suspend_fault_mask_;
    if (this->link_pump_ready_seen_) show_none = true;
    if (lf.empty()) show_none = true;
    const std::string shown = show_none ? std::string("None") : lf;
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
  if (this->suspended_) {
    // Ahead of every other rung, including the ready check: a link we took down
    // on purpose is not Unreachable, not Reconnecting, and not a fault. The
    // operator glancing at Home Assistant should see the state they asked for.
    state = "Suspended";
  } else if (this->session_.is_ready()) {
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
  initial_read_started_ms_ = millis();
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

  // Measure pump clock drift. A READ only -- the write is check_and_sync_time()'s.
  //
  // This leg used to submit the sync as well, and could not: sync_pump_clock()
  // goes through check_ready() -> is_state_synchronized(), which needs the
  // control cache that sync_cache_async() fills from the t=5000 leg below. At
  // t=2000 that cache is empty on every connection, cold boot and reconnect
  // alike, so the submission was refused 100% of the time -- visible on the
  // bench as "Command rejected: pump state is not yet fully synchronized
  // (sync_pump_clock)" at every boot. check_and_sync_time() then wrote the
  // clock on a later update() anyway, which is why nothing looked broken.
  //
  // Rather than move this leg past t=5000 to race the same gate from the other
  // side, the write is left where it already works and this leg does what it
  // can do at t=2000: read the pump's clock and publish the pre-sync drift.
  this->set_timeout(2000, [this, gen]() {
    if (gen != this->read_chain_gen_) return;
    // First attempt only: the drift figure is "how far out was the pump when we
    // found it", so a re-armed chain must not overwrite it with a reading taken
    // after a sync has already corrected the pump.
    if (this->initial_clock_sync_started_) {
      ESP_LOGD(TAG, "Skipping pump clock drift read on read-chain retry");
      return;
    }
    this->initial_clock_sync_started_ = true;
    ESP_LOGD(TAG, "Measuring pump clock drift...");

    // First read the pump clock to measure drift
    this->time_service_.get_clock_async([this, gen](ESPTime pump_time) {
      // The generation check every other leg of this chain has, and the one
      // this leg was missing (issue #259). Since reset() started failing what
      // it abandons, a disconnect while this read is queued invokes this
      // callback with an invalid time and the else-branch below publishes NAN
      // -- to a user-facing sensor, on every dropped link, for as long as the
      // read sits in the queue behind the rest of the chain. That is a wide
      // window, not a race: a queued command has no timeout of its own, which
      // is the whole subject of #259.
      //
      // It is also the clobber this leg already guards against one level up.
      // The note on the timeout above says the drift figure is "how far out was
      // the pump when we found it" and must not be overwritten by a later
      // reading; overwriting it with NAN is the same defect with a worse value.
      if (gen != this->read_chain_gen_) return;
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

      // No sync submitted here; see the note above this timeout.
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
    // (e.g., BLE connection persisted through ESP32 restart, so the session
    // was already ready and on_session_stabilized_() never ran),
    // trigger them now.
    if (!initial_data_read_done_) {
      ESP_LOGD(TAG,
               "Session ready but initial data not yet read - triggering now");
      // Already running on the re-arm path, where only the read chain is being
      // retried; calling start() again would log a misleading warning.
      if (!telemetry_service_.is_running()) {
        telemetry_service_.start();
      }
      trigger_initial_data_reads();
    } else {
      // If we are initialized, evaluate cache validity to update Ready sensor
      if (!this->link_pump_ready_seen_ && is_state_synchronized()) {
        ESP_LOGI(TAG, "Cache synchronized: pump is fully READY for control");
        // The latch, and the readiness watchdog's only off switch. Set from the
        // predicate rather than from the sensor: `ready_status` is optional, and
        // a watchdog that treats an undeclared entity as "never ready" recycles
        // a healthy link forever (issue #211 review).
        this->link_pump_ready_seen_ = true;
        // ...and with it the suspend mask, which has been holding the fault
        // surface at "None" across the reconnect (issue #243).
        this->suspend_fault_mask_ = false;
        if (ready_sensor_ && !ready_sensor_->state)
          ready_sensor_->publish_state(true);
        // The one thing that refutes a readiness fault, so the one thing that
        // releases it. Also the only evidence that the window was merely too
        // short rather than the link being stuck, so the backoff resets here
        // and nowhere else.
        this->ble_manager_.on_pump_ready();
        this->link_ready_timeout_current_ms_ = this->link_ready_timeout_ms_;
        this->link_recycles_without_ready_ = 0;
      }

      // Publish the run state and repair a dead schedule (issue #124). Gated on
      // full sync so it never acts on a half-populated cache; this is also the
      // only path that reconciles run state at boot.
      if (is_state_synchronized()) {
        reconcile_run_state_();
      }

      if (core::should_rearm_initial_read(
              session_.is_ready(), initial_data_read_done_,
              is_state_synchronized(), chain_products_complete_(), millis(),
              initial_read_started_ms_, initial_read_retry_interval_ms_)) {
        // The chain ran but its reads never landed, and only a disconnect would
        // otherwise clear the latch — so without this the device stays
        // half-initialised for as long as the link stays up. Bumping the
        // generation first retires any still-pending timers from the failed
        // attempt, so the retry cannot double up with them.
        ESP_LOGW(TAG,
                 "Initial data reads incomplete after %" PRIu32
                 " s (device_info=%d statistics=%d caches=%d) - re-arming the "
                 "read chain",
                 initial_read_retry_interval_ms_ / 1000,
                 (int) device_info_read_ok_, (int) statistics_read_ok_,
                 (int) is_state_synchronized());
        read_chain_gen_++;
        initial_data_read_done_ = false;
        // Re-prove these on the retry rather than inheriting a stale verdict.
        device_info_read_ok_ = false;
        statistics_read_ok_ = false;
        initial_read_retry_interval_ms_ = core::next_initial_read_backoff_ms(
            initial_read_retry_interval_ms_, INITIAL_READ_RETRY_MAX_MS);
      } else if (chain_products_complete_() && is_state_synchronized()) {
        // Landed: forget the backoff so a later reconnect starts fresh.
        initial_read_retry_interval_ms_ = INITIAL_READ_SYNC_TIMEOUT_MS;
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
    // Down on purpose is not worth a warning every ten seconds -- 27 lines in
    // one 4.5 minute session on the reporter's node (issue #243).
    if (this->suspended_) {
      ESP_LOGD(TAG, "Skipping polls - link suspended");
    } else {
      ESP_LOGW(TAG, "Skipping polls - not ready");
    }
  }
}

bool AlphaHwrComponent::submit_clock_sync_(const char *reason) {
  // Ask quietly first. sync_pump_clock() goes through check_ready(), which
  // WARNs when the pump state is not synchronized -- right for a user or an
  // automation that asked for something, wrong for a poll that runs every 10 s
  // and is simply early. The cache fills a few seconds into each connection, so
  // without this every boot logs the rejection once or twice.
  if (!this->is_state_synchronized()) {
    ESP_LOGD(TAG, "%s pump clock sync deferred: pump state not synchronized yet", reason);
    return false;
  }

  // Assume the sync will not confirm. The result callback promotes this to the
  // full day; every other path -- rejected, timeout, superseded, or never
  // submitted at all -- leaves the short interval in place.
  this->time_sync_interval_ms_ = TIME_SYNC_RETRY_MS;

  const bool submitted = this->sync_pump_clock(
      "clock_sync", [this, reason](bool success) {
        if (!success) {
          ESP_LOGW(TAG, "%s pump clock sync did not confirm - retrying in %u min",
                   reason, static_cast<unsigned>(TIME_SYNC_RETRY_MS / 60000));
          return;
        }
        ESP_LOGD(TAG, "%s pump clock sync confirmed", reason);
        this->time_sync_interval_ms_ = TIME_SYNC_INTERVAL_MS;

#ifdef USE_TEXT_SENSOR
        // "Last Clock Sync" now means what its name says. It used to be
        // stamped from a callback that was hardcoded true, so it advanced
        // whether or not the pump ever received the write.
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
      });

  if (!submitted) {
    // Nothing was written and no BLE traffic was spent. Only one cause reaches
    // here now: the pump's state cache is not synchronized yet. The other two
    // this used to name -- no time_id, and a time source that never answered --
    // are turned away by check_and_sync_time()'s gate before this runs. Leave the stamp alone so the next update() (10 s) tries
    // again, rather than backing off 15 minutes over a check that costs nothing
    // to repeat. DEBUG, not WARN: on the bench this fires twice at every boot
    // and is followed ten seconds later by a sync that works.
    ESP_LOGD(TAG, "%s pump clock sync not attempted yet; retrying shortly", reason);
    return false;
  }

  // Stamp the ATTEMPT. See the field's declaration: throttling on success
  // alone would let an unconfirmable clock collect a write every 10 s.
  uint32_t stamp = millis();
  if (stamp == 0) stamp = 1;  // 0 is the "never attempted" sentinel
  this->last_time_sync_timestamp_ = stamp;
  return true;
}

void AlphaHwrComponent::warn_clock_not_syncing_(const char *why) {
  // Throttled, because the conditions that reach here persist for the life of
  // the run -- an unthrottled warning on a 10 s poll would be the same spin it
  // replaces, only louder. Hourly is often enough that a log reader attaching
  // at any point sees it, and rare enough to be free.
  const uint32_t now = millis();
  if (this->clock_warn_last_ms_ != 0 &&
      (now - this->clock_warn_last_ms_) < CLOCK_WARN_INTERVAL_MS) {
    return;
  }
  this->clock_warn_last_ms_ = (now == 0) ? 1 : now;  // 0 is the "never warned" sentinel
  ESP_LOGW(TAG, "Pump clock is not being synced: %s", why);
  ESP_LOGW(TAG, "  Schedule windows run on the pump's own RTC, which drifts");
}

void AlphaHwrComponent::check_and_sync_time() {
  // Decide whether a sync is even possible before running the retry machinery
  // below, which is built for conditions that resolve on their own. See
  // clock_sync_gate.h for why the three non-syncing states have to be told
  // apart rather than collapsed into one early return.
  const core::ClockSyncAction action = core::clock_sync_action(
      this->has_wall_clock_(), this->time_service_.wall_clock_is_set(), millis(),
      core::CLOCK_SOURCE_GRACE_MS);
  // Switched rather than reduced to a ternary on purpose: the ternary that
  // stood here mapped every warning state that was not WARN_NO_TIME_ID onto the
  // "time source has not answered" message, so a state added later would have
  // been reported to the user as the wrong cause, silently. A switch with no
  // default makes that a compiler diagnostic instead.
  switch (action) {
    case core::ClockSyncAction::SYNC:
      break;
    case core::ClockSyncAction::WAIT:
      return;
    case core::ClockSyncAction::WARN_NO_TIME_ID:
      this->warn_clock_not_syncing_("no time_id is configured");
      return;
    case core::ClockSyncAction::WARN_NO_SOURCE:
      this->warn_clock_not_syncing_("its configured time source has not answered");
      return;
  }

  uint32_t now = millis();

  // Handle millis() rollover (every ~49 days)
  if (now < last_time_sync_timestamp_) {
    ESP_LOGD(TAG, "millis() rollover detected, resetting time sync tracking");
    last_time_sync_timestamp_ = 0;
  }

  if (last_time_sync_timestamp_ != 0 &&
      (now - last_time_sync_timestamp_) < time_sync_interval_ms_) {
    return;
  }

  ESP_LOGD(TAG, "Pump clock sync due");
  this->submit_clock_sync_("Daily");
}

void AlphaHwrComponent::read_device_info() {
  device_info_service_.read_device_info_async([this](bool success) {
    if (success) {
      ESP_LOGI(TAG, "Device info read completed successfully");
      this->device_info_read_ok_ = true;
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
          this->statistics_read_ok_ = true;
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

void AlphaHwrComponent::on_session_stabilized_() {
  // The transport's write callback dereferences parent_ unchecked (see
  // set_write_callback() in setup()), and telemetry_service_.start() opens the
  // path to it. authenticate() carried this guard; it is kept for the same
  // reason, not because this callback is otherwise reachable without a client.
  if (!parent_) {
    ESP_LOGW(TAG, "Parent BLE client not available");
    return;
  }

  session_.on_ready();
  ESP_LOGI(TAG, "Pump stabilized - session ready");

  // Release an auth-failure hold on the fault string. Nothing else releases it
  // once the pump has stopped producing successful AUTH_CMPL events, and the
  // string is displayed only while the session is not ready -- so a hold that
  // outlives READY cannot report the pairing failure any more, it can only mask
  // the next outage's cause with it (failure_hold.h). This is BLE pairing, not
  // the removed GENI sequence. Unlike the link-status counters below, it is
  // safe on a deaf link: if the link is deaf, the watchdog writes the true
  // reason 60 s later, which is the point.
  ble_manager_.on_session_ready();

  telemetry_service_.start();

  // Pump Link Status: deliberately does NOT mark this a working link. Reaching
  // READY now proves strictly less than it used to -- not one frame has been
  // exchanged, where before at least a chain of timers had run. Clearing
  // link_consecutive_failures_ here would reset the count on every watchdog
  // recycle, so a permanently deaf pump could never accumulate the LINK_FAIL_K
  // failures that surface the "Reconnecting" rung -- it would flip between
  // Connected and Connecting forever. The reset lives on the notification path
  // instead, where inbound data actually proves the link works.
  evaluate_link_status();

  // Trigger the one-time data read chain
  trigger_initial_data_reads();
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
