import logging
import os
import subprocess

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import (
    binary_sensor,
    ble_client,
    esp32_ble_tracker,
    sensor,
    text_sensor,
    time,
)
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    STATE_CLASS_TOTAL_INCREASING,
    UNIT_CELSIUS,
    UNIT_WATT,
)

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]


def _component_source_revision() -> str:
    """`git describe` of this component's own source tree, resolved at codegen.

    Identifies which build is installed (issue #124). The release version can't:
    it only changes at a release, so every build between two releases reports
    the same value and installs cannot be correlated with behavior changes in
    Home Assistant history — which is how a three-day outage went unattributed.

    ESPHome clones `github://` sources with git, and a `type: local` source is
    the developer's working tree, so both resolve here (a shallow clone without
    tags still yields the commit via `--always`). Any failure — no git, tarball
    install, unreadable directory — degrades to "unknown"; the firmware build
    timestamp published alongside it still separates installs. A diagnostic must
    never fail a build.
    """
    try:
        result = subprocess.run(
            ["git", "describe", "--tags", "--always", "--dirty"],
            cwd=os.path.dirname(os.path.abspath(__file__)),
            capture_output=True,
            text=True,
            timeout=5,
            check=False,
        )
    except (OSError, subprocess.SubprocessError):
        return "unknown"
    revision = result.stdout.strip()
    if result.returncode != 0 or not revision:
        return "unknown"
    return revision


alpha_hwr_ns = cg.esphome_ns.namespace("alpha_hwr")
AlphaHwrComponent = alpha_hwr_ns.class_("AlphaHwrComponent", cg.PollingComponent, ble_client.BLEClientNode)

CONF_FLOW = "flow"
CONF_HEAD = "head"
CONF_POWER = "power"
CONF_RPM = "rpm"
CONF_TEMP_MEDIA = "temp_media"
CONF_TEMP_PCB = "temp_pcb"
CONF_TEMP_CONTROL_BOX = "temp_control_box"
CONF_VOLTAGE = "voltage"
CONF_VOLTAGE_DC = "voltage_dc"
CONF_CURRENT = "current"
CONF_INLET_PRESSURE = "inlet_pressure"
CONF_HEAD_RATE = "head_rate"
CONF_PAIRING_STATUS = "pairing_status"
CONF_READY_STATUS = "ready_status"
# Renamed from `enable_pairing` (issue #245). The old name reads as a property
# of the LINK -- "this node will not bond" -- and that is a guarantee no ESPHome
# component can make: when the pump initiates, `BLEClientBase::gap_event_handler()`
# has already consented on our behalf, unconditionally, before this component
# sees the event. A node with the old option set to false was observed bonding
# four times in twenty minutes, 252 ms after logging "Skipping encryption
# request - pairing disabled".
#
# What the option actually governs is OUR side of the negotiation: whether this
# component initiates pairing and configures security parameters. `initiate_`
# carries that distinction in the name, which is where the wrong expectation was
# being formed.
CONF_INITIATE_PAIRING = "initiate_pairing"
CONF_ENABLE_PAIRING = "enable_pairing"
CONF_RECONNECT_SETTLE_TIME = "reconnect_settle_time"
CONF_CONTROL_STATE_POLL_INTERVAL = "control_state_poll_interval"
CONF_DATA_TIMEOUT = "data_timeout"
CONF_READY_TIMEOUT = "ready_timeout"
CONF_READY_RECYCLE = "ready_recycle"

# `ready_recycle: true` -- recycle without a bound. Mirrors
# AlphaHwrComponent::READY_RECYCLE_FOREVER; the user never types this number.
READY_RECYCLE_FOREVER = 0xFFFFFFFF
# An upper bound on what anyone can usefully type. Each recycle takes another
# run at the window that can erase a bond (issue #14), so a config asking for
# hundreds is far likelier to be a mistake than an intention, and "unbounded"
# already has a spelling: `true`.
READY_RECYCLE_MAX = 100


def validate_ready_recycle(value):
    """`ready_recycle` is a COUNT, and still accepts the boolean it used to be.

    Issue #257: as a boolean this was off-or-forever, and the case the reporter
    cared about is neither -- if a link that will not finish its opening reads
    is a one-off glitch, one reconnect clears it, and if it is not, another
    fifty will not either while each takes another run at the bond-erase
    window. So `0` never recycles, `N` recycles at most N consecutive times and
    then leaves the fault standing.

    A boolean still means what it always meant, so no existing config changes
    behaviour: `false` is 0 and `true` is unbounded. Booleans are tested FIRST
    and deliberately -- `bool` is a subclass of `int` in Python, so an integer
    validator would silently accept `true` as the number 1 and quietly turn
    "recycle forever" into "recycle once" in configs written before this issue.
    """
    if isinstance(value, bool):
        return READY_RECYCLE_FOREVER if value else 0
    if isinstance(value, str):
        # YAML quoting, or the words ESPHome accepts for a boolean elsewhere.
        try:
            return READY_RECYCLE_FOREVER if cv.boolean(value) else 0
        except cv.Invalid:
            pass
    return cv.int_range(min=0, max=READY_RECYCLE_MAX)(value)


CONF_ALARMS = "alarms"
CONF_WARNINGS = "warnings"
CONF_SCHEDULE_HASH = "schedule_hash"
CONF_SCHEDULE_LAYERS = [f"schedule_layer_{n}" for n in range(5)]
CONF_CONTROL_MODE = "control_mode"
CONF_PUMP_RUN_STATE = "pump_run_state"
CONF_SCHEDULE_STALLED = "schedule_stalled"
CONF_COMPONENT_BUILD = "component_build"
CONF_SERIAL_NUMBER = "serial_number"
CONF_SOFTWARE_VERSION = "software_version"
CONF_HARDWARE_VERSION = "hardware_version"
CONF_BLE_VERSION = "ble_version"
CONF_PRODUCT_NAME = "product_name"
CONF_PRODUCT_VERSION = "product_version"
CONF_SINGLE_EVENTS = "single_events"
CONF_VACATION = "vacation"
CONF_EVENT_LOG = "event_log"
CONF_HISTORY = "history"
CONF_CYCLE_TIMESTAMPS = "cycle_timestamps"
CONF_START_COUNT = "start_count"
CONF_OPERATING_HOURS = "operating_hours"
CONF_CLOCK_DIFF = "clock_diff"
CONF_LAST_CLOCK_SYNC = "last_clock_sync"
CONF_PUMP_LINK_STATUS = "pump_link_status"
CONF_LINK_RECYCLES = "link_recycles"
CONF_LINK_MAX_GAP = "link_max_gap"
# The tail histogram (issue #176 part 1). These names mirror
# LINK_GAP_THRESHOLDS_MS in link_watchdog.h, and the coupling is enforced rather
# than trusted: each generated key resolves to set_link_gaps_over_<T>s_sensor()
# in alpha_hwr.h, which static_asserts that its index really does hold that
# threshold. Change one side without the other and the build fails, instead of
# an entity quietly reporting a rung it is not counting.
LINK_GAP_THRESHOLDS_S = [15, 20, 30, 45, 60, 90]
CONF_LINK_GAPS_OVER = [f"link_gaps_over_{t}s" for t in LINK_GAP_THRESHOLDS_S]
CONF_LINK_GAPS_TRUNCATED = "link_gaps_truncated"
CONF_LINK_WATCH_TIME = "link_watch_time"
CONF_PUMP_LAST_LINK_FAILURE = "pump_last_link_failure"
CONF_PUMP_CLOCK_DST = "pump_clock_dst"
CONF_FLOW_LIMITER = "flow_limiter"
CONF_FLOW_LIMITED = "flow_limited"
CONF_TIME_ID = "time_id"


def resolve_pairing_alias(config):
    """Fold the old `enable_pairing` spelling onto `initiate_pairing` (#245).

    Both are optional in the schema so that either may be written, and exactly
    one reaches `to_code()`. A config setting both to the same value is fine and
    silent; setting them to DIFFERENT values is refused rather than resolved,
    because there is no reading of that config which is obviously what the
    author meant, and guessing would silently pair or not pair a pump.
    """
    legacy = config.pop(CONF_ENABLE_PAIRING, None)
    current = config.get(CONF_INITIATE_PAIRING)
    if legacy is not None and current is not None and legacy != current:
        raise cv.Invalid(
            f"{CONF_ENABLE_PAIRING} and {CONF_INITIATE_PAIRING} disagree "
            f"({legacy} vs {current}). {CONF_ENABLE_PAIRING} is the old name for "
            f"the same option; set only {CONF_INITIATE_PAIRING}."
        )
    if current is None:
        config[CONF_INITIATE_PAIRING] = legacy if legacy is not None else False
    return config


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(AlphaHwrComponent),
            cv.Required("ble_client_id"): cv.use_id(ble_client.BLEClient),
            cv.Optional(CONF_TIME_ID): cv.use_id(time.RealTimeClock),
            cv.Optional(CONF_INITIATE_PAIRING): cv.boolean,
            # The old spelling, still accepted so no config breaks. Kept
            # undocumented rather than deprecated-with-a-warning: it means
            # exactly what it always meant, and the rename is about what the
            # name IMPLIES rather than about a behaviour change (issue #245).
            cv.Optional(CONF_ENABLE_PAIRING): cv.boolean,
            # 2s default: covers the pump's measured post-boot vulnerability window
            # (bounded at 320-720ms, during which an encryption request fails with
            # 0x61 and erases the bond) with ~2.8x margin even assuming zero
            # host-side processing time. See issue #14 for the measurements.
            cv.Optional(CONF_RECONNECT_SETTLE_TIME, default="2s"): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_CONTROL_STATE_POLL_INTERVAL, default="30s"): cv.positive_time_period_milliseconds,
            # Inbound-data watchdog: tear the BLE link down when the pump stops
            # answering while the session still reports itself connected. A READY
            # link is polled every 10s (five telemetry registers plus the schedule),
            # so 60s is six missed poll cycles. 0 disables it. See
            # components/alpha_hwr/link_watchdog.h.
            cv.Optional(CONF_DATA_TIMEOUT, default="60s"): cv.positive_time_period_milliseconds,
            # Readiness (progress) watchdog, issue #211. The data watchdog above
            # watches liveness and is re-armed by every inbound notification, so
            # a session stuck while the pump keeps volunteering telemetry is
            # invisible to it. This one is timed from connection-open and is
            # cleared only by the pump actually becoming usable.
            #
            # How long a connection may go without the pump becoming usable
            # before the component says so. Defaults ON, because saying so costs
            # nothing: by itself this only latches a fault string and logs.
            #
            # 300s against a fresh connection that reaches ready in about 22s on
            # a bonded pump -- measured by setting the window short on hardware
            # and watching which values fired (10s fired, 20s fired, 40s did
            # not). A first pairing is still untimed, so err high rather than
            # low. 0 disables it.
            cv.Optional(CONF_READY_TIMEOUT, default="300s"): cv.positive_time_period_milliseconds,
            # ...and whether reaching that bound also tears the link down so the
            # normal reconnect runs. Defaults OFF, and the asymmetry between
            # these two options is the point.
            #
            # Naming the fault is free. Recycling is not: every forced reconnect
            # takes another run at the encryption-on-open window that can erase
            # the pump's bond (issue #14), and a bond erased that way needs
            # physical access to the pump to restore (issue #230). On a node
            # that never becomes ready it would do that on an escalating
            # schedule indefinitely.
            #
            # Whether such a node exists in a supported configuration is issue
            # #244 -- an attempt to measure it was confounded three ways at once
            # (a pre-release build, pairing enabled rather than defaulted, and a
            # signal at the noise floor) and it bonded within 252 ms regardless
            # (issue #245). So the diagnosis ships on and the remedy waits for
            # someone who wants it and can see their node reaches ready today.
            cv.Optional(CONF_READY_RECYCLE, default=False): validate_ready_recycle,
            cv.Optional(CONF_FLOW): sensor.sensor_schema(
                unit_of_measurement="m³/h",
                accuracy_decimals=3,
                device_class="volume_flow_rate",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # Head is published in meters of head (the pump's native unit and the
            # same unit as the pressure setpoints). "m" is not a valid Home
            # Assistant `pressure` device_class unit, so the class is `distance`:
            # meters of head is dimensionally a length, and `distance` accepts both
            # m and ft. This is display metadata only — the published value stays
            # meters — but it gives Home Assistant the unit picker, so the value can
            # be shown in feet, the unit the Grundfos manual leads with for this
            # pump (§13: "Head (H) 15-55: max. 18 ft (5.5 m)"). See issue #157.
            cv.Optional(CONF_HEAD): sensor.sensor_schema(
                unit_of_measurement="m",
                accuracy_decimals=2,
                device_class="distance",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_POWER): sensor.sensor_schema(
                unit_of_measurement=UNIT_WATT,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_POWER,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_RPM): sensor.sensor_schema(
                unit_of_measurement="RPM",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMP_MEDIA): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMP_PCB): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_TEMP_CONTROL_BOX): sensor.sensor_schema(
                unit_of_measurement=UNIT_CELSIUS,
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_TEMPERATURE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_VOLTAGE): sensor.sensor_schema(
                unit_of_measurement="V",
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_VOLTAGE_DC): sensor.sensor_schema(
                unit_of_measurement="V",
                accuracy_decimals=1,
                device_class=DEVICE_CLASS_VOLTAGE,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_CURRENT): sensor.sensor_schema(
                unit_of_measurement="A",
                accuracy_decimals=2,
                device_class=DEVICE_CLASS_CURRENT,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_INLET_PRESSURE): sensor.sensor_schema(
                unit_of_measurement="bar",
                accuracy_decimals=2,
                device_class="pressure",
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_HEAD_RATE): sensor.sensor_schema(
                unit_of_measurement="m/s",
                accuracy_decimals=4,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:gauge",
            ),
            cv.Optional(CONF_PAIRING_STATUS): binary_sensor.binary_sensor_schema(
                device_class=DEVICE_CLASS_CONNECTIVITY,
            ),
            cv.Optional(CONF_READY_STATUS): binary_sensor.binary_sensor_schema(
                icon="mdi:check-network-outline",
            ),
            cv.Optional(CONF_ALARMS): text_sensor.text_sensor_schema(
                icon="mdi:alert-circle",
            ),
            cv.Optional(CONF_WARNINGS): text_sensor.text_sensor_schema(
                icon="mdi:alert",
            ),
            cv.Optional(CONF_SCHEDULE_HASH): text_sensor.text_sensor_schema(
                icon="mdi:pound",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            **{
                cv.Optional(key): text_sensor.text_sensor_schema(
                    icon="mdi:calendar-export",
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                )
                for key in CONF_SCHEDULE_LAYERS
            },
            cv.Optional(CONF_CONTROL_MODE): text_sensor.text_sensor_schema(
                icon="mdi:cog",
            ),
            # Run state: off / engaged / scheduled / stalled (issue #124). The only
            # entity that separates AUTO from STOP while the schedule is enabled —
            # "Engage Pump" reads off for both.
            cv.Optional(CONF_PUMP_RUN_STATE): text_sensor.text_sensor_schema(
                icon="mdi:pump",
            ),
            # Which build of this component is installed: source revision + firmware
            # build timestamp, so HA history can be correlated with installs. The
            # release version alone only changes at a release (issue #124).
            cv.Optional(CONF_COMPONENT_BUILD): text_sensor.text_sensor_schema(
                icon="mdi:source-commit",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Alarms the illegal "schedule enabled but pump stopped" state, which
            # silently loses every window (issue #124).
            cv.Optional(CONF_SCHEDULE_STALLED): binary_sensor.binary_sensor_schema(
                device_class="problem",
                icon="mdi:calendar-alert",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SERIAL_NUMBER): text_sensor.text_sensor_schema(
                icon="mdi:barcode",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SOFTWARE_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:update",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_HARDWARE_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:chip",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_BLE_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:bluetooth",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PRODUCT_NAME): text_sensor.text_sensor_schema(
                icon="mdi:information",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_PRODUCT_VERSION): text_sensor.text_sensor_schema(
                icon="mdi:tag",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_SINGLE_EVENTS): text_sensor.text_sensor_schema(
                icon="mdi:calendar-star",
            ),
            cv.Optional(CONF_VACATION): text_sensor.text_sensor_schema(
                icon="mdi:airplane",
            ),
            cv.Optional(CONF_EVENT_LOG): text_sensor.text_sensor_schema(
                icon="mdi:history",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_HISTORY): text_sensor.text_sensor_schema(
                icon="mdi:chart-line",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_CYCLE_TIMESTAMPS): text_sensor.text_sensor_schema(
                icon="mdi:clock-outline",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            cv.Optional(CONF_START_COUNT): sensor.sensor_schema(
                accuracy_decimals=0,
                icon="mdi:counter",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_OPERATING_HOURS): sensor.sensor_schema(
                unit_of_measurement="h",
                accuracy_decimals=1,
                icon="mdi:timer-sand",
                device_class="duration",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_CLOCK_DIFF): sensor.sensor_schema(
                unit_of_measurement="s",
                accuracy_decimals=0,
                icon="mdi:timer-sand",
                device_class="duration",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_LAST_CLOCK_SYNC): text_sensor.text_sensor_schema(
                icon="mdi:clock-check",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Link diagnostics (issue #176). Both read 0 on a healthy link, so an
            # automation can threshold on the recycle count rather than having to
            # notice a flap cadence live, and the observed gap is what a
            # data-driven data_timeout default has to be chosen from.
            cv.Optional(CONF_LINK_RECYCLES): sensor.sensor_schema(
                icon="mdi:restart-alert",
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            cv.Optional(CONF_LINK_MAX_GAP): sensor.sensor_schema(
                unit_of_measurement="s",
                icon="mdi:timer-alert-outline",
                accuracy_decimals=1,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_MEASUREMENT,
            ),
            # The tail histogram, its trust check, and its denominator (issue #176
            # part 1). link_max_gap above is one point of this distribution; these
            # give its shape, which is what "how often would a budget of T have
            # fired" actually needs.
            #
            # total_increasing rather than measurement, and that is the reason these
            # are numeric counters at all: they are RAM values that restart at every
            # boot, and Home Assistant's long-term statistics recognise the reset and
            # keep accumulating. A run measured in weeks survives the OTAs and
            # crashes it will certainly meet; a running maximum does not.
            **{
                cv.Optional(key): sensor.sensor_schema(
                    icon="mdi:timer-alert-outline",
                    accuracy_decimals=0,
                    entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                    state_class=STATE_CLASS_TOTAL_INCREASING,
                )
                for key in CONF_LINK_GAPS_OVER
            },
            cv.Optional(CONF_LINK_GAPS_TRUNCATED): sensor.sensor_schema(
                icon="mdi:content-cut",
                accuracy_decimals=0,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional(CONF_LINK_WATCH_TIME): sensor.sensor_schema(
                unit_of_measurement="s",
                icon="mdi:timer-outline",
                accuracy_decimals=0,
                device_class="duration",
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                state_class=STATE_CLASS_TOTAL_INCREASING,
            ),
            cv.Optional(CONF_PUMP_LINK_STATUS): text_sensor.text_sensor_schema(
                icon="mdi:bluetooth-connect",
            ),
            cv.Optional(CONF_PUMP_LAST_LINK_FAILURE): text_sensor.text_sensor_schema(
                icon="mdi:alert-circle-outline",
            ),
            # Whether the pump's own daylight-saving rule matches this node's
            # timezone (issue #286). Optional, and the read is only issued when
            # it is configured -- a node that does not ask does not spend a
            # round trip per connection on it.
            cv.Optional(CONF_PUMP_CLOCK_DST): text_sensor.text_sensor_schema(
                icon="mdi:sun-clock-outline",
            ),
            # The pump's MaxFlow/MinFlow limiters (issue #274). Optional, and
            # the reads are only issued when one of these is configured -- five
            # frames per connection and three per control poll otherwise buy
            # nothing.
            cv.Optional(CONF_FLOW_LIMITER): text_sensor.text_sensor_schema(
                icon="mdi:speedometer-slow",
            ),
            cv.Optional(CONF_FLOW_LIMITED): binary_sensor.binary_sensor_schema(
                icon="mdi:speedometer-slow",
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(esp32_ble_tracker.ESP_BLE_DEVICE_SCHEMA)
    .add_extra(resolve_pairing_alias)
)


def _warn_if_histogram_cannot_fill(config):
    """Say at config time when the gap histogram is censored by data_timeout.

    The watchdog closes a quiet interval as soon as the budget expires, so every
    rung at or above `data_timeout` reads a structural zero however badly the
    pump behaves -- and a rung exactly at it silently becomes a recycle count
    rather than a gap count. A measurement run started that way produces
    reassuring numbers and settles nothing.

    The component also warns at boot, but that fires at setup_priority::DATA,
    before the API server is up -- so it reaches the serial console only, and
    nobody flashing over the air ever sees it (verified on the bench: the
    warning is absent from an `esphome logs` stream of a boot that emitted it).
    Here it lands in `esphome config` and `esphome compile` output, where the
    person choosing the value is actually looking.
    """
    declared = [key for key in CONF_LINK_GAPS_OVER if key in config]
    if not declared:
        return config
    budget_ms = config[CONF_DATA_TIMEOUT].total_milliseconds
    if budget_ms == 0:
        return config  # nothing recycles, so nothing truncates an interval
    # Above the budget and equal to it fail differently, and lumping them
    # together is wrong about the second. A rung above the budget is a
    # structural zero. A rung AT the budget does increment -- but only on the
    # intervals the watchdog itself cut off, so it stops counting quiet periods
    # and starts counting recycles. That one is the more insidious of the two,
    # because the number looks alive.
    declared = [
        (threshold, key)
        for threshold, key in zip(LINK_GAP_THRESHOLDS_S, CONF_LINK_GAPS_OVER, strict=True)
        if key in config
    ]
    above = [t for t, _ in declared if t * 1000 > budget_ms]
    at_budget = [t for t, _ in declared if t * 1000 == budget_ms]
    if above:
        _LOGGER.warning(
            "alpha_hwr: data_timeout is %ss, so the %s gap counter(s) cannot fill -- "
            "the watchdog truncates the interval first. Raise data_timeout (600s) "
            "for a measurement run; see docs/configuration.md",
            budget_ms // 1000,
            ", ".join(f"{t}s" for t in above),
        )
    if at_budget:
        _LOGGER.warning(
            "alpha_hwr: the %s gap counter(s) equal data_timeout, so the only intervals "
            "that can reach them are ones the watchdog cut off -- they count recycles, "
            "not quiet periods. Raise data_timeout (600s) for a measurement run",
            ", ".join(f"{t}s" for t in at_budget),
        )
    return config


FINAL_VALIDATE_SCHEMA = _warn_if_histogram_cannot_fill


async def to_code(config):
    ble_client_var = await cg.get_variable(config["ble_client_id"])
    var = cg.new_Pvariable(config[CONF_ID], ble_client_var)
    await cg.register_component(var, config)

    if CONF_TIME_ID in config:
        time_ = await cg.get_variable(config[CONF_TIME_ID])
        cg.add(var.set_time_id(time_))

    # Set pairing enabled flag
    cg.add(var.set_pairing_enabled(config[CONF_INITIATE_PAIRING]))

    cg.add(var.set_reconnect_settle_time(config[CONF_RECONNECT_SETTLE_TIME]))
    cg.add(var.set_data_timeout(config[CONF_DATA_TIMEOUT]))
    cg.add(var.set_ready_timeout(config[CONF_READY_TIMEOUT]))
    cg.add(var.set_ready_recycle_limit(config[CONF_READY_RECYCLE]))
    if config[CONF_RECONNECT_SETTLE_TIME].total_milliseconds > 0:
        # Register as an esp32_ble_tracker listener (at codegen, so the count
        # macro that enables the listener path is defined) — this is what makes
        # parse_device() receive scan results, used to time the reconnect settle
        # window from the pump's reappearance. Only when the feature is enabled.
        await esp32_ble_tracker.register_ble_device(var, config)

    if CONF_FLOW in config:
        sens = await sensor.new_sensor(config[CONF_FLOW])
        cg.add(var.set_flow_sensor(sens))

    if CONF_HEAD in config:
        sens = await sensor.new_sensor(config[CONF_HEAD])
        cg.add(var.set_head_sensor(sens))

    if CONF_POWER in config:
        sens = await sensor.new_sensor(config[CONF_POWER])
        cg.add(var.set_power_sensor(sens))

    if CONF_RPM in config:
        sens = await sensor.new_sensor(config[CONF_RPM])
        cg.add(var.set_rpm_sensor(sens))

    if CONF_TEMP_MEDIA in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_MEDIA])
        cg.add(var.set_temp_media_sensor(sens))

    if CONF_TEMP_PCB in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_PCB])
        cg.add(var.set_temp_pcb_sensor(sens))

    if CONF_TEMP_CONTROL_BOX in config:
        sens = await sensor.new_sensor(config[CONF_TEMP_CONTROL_BOX])
        cg.add(var.set_temp_control_box_sensor(sens))

    if CONF_VOLTAGE in config:
        sens = await sensor.new_sensor(config[CONF_VOLTAGE])
        cg.add(var.set_voltage_sensor(sens))

    if CONF_VOLTAGE_DC in config:
        sens = await sensor.new_sensor(config[CONF_VOLTAGE_DC])
        cg.add(var.set_voltage_dc_sensor(sens))

    if CONF_CURRENT in config:
        sens = await sensor.new_sensor(config[CONF_CURRENT])
        cg.add(var.set_current_sensor(sens))

    if CONF_INLET_PRESSURE in config:
        sens = await sensor.new_sensor(config[CONF_INLET_PRESSURE])
        cg.add(var.set_inlet_pressure_sensor(sens))

    if CONF_HEAD_RATE in config:
        sens = await sensor.new_sensor(config[CONF_HEAD_RATE])
        cg.add(var.set_head_rate_sensor(sens))

    if CONF_PAIRING_STATUS in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_PAIRING_STATUS])
        cg.add(var.set_pairing_status_binary_sensor(sens))

    if CONF_READY_STATUS in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_READY_STATUS])
        cg.add(var.set_ready_binary_sensor(sens))

    if CONF_ALARMS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_ALARMS])
        cg.add(var.set_alarms_text_sensor(sens))

    if CONF_WARNINGS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_WARNINGS])
        cg.add(var.set_warnings_text_sensor(sens))

    if CONF_SCHEDULE_HASH in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCHEDULE_HASH])
        cg.add(var.set_schedule_hash_text_sensor(sens))

    for layer, key in enumerate(CONF_SCHEDULE_LAYERS):
        if key in config:
            sens = await text_sensor.new_text_sensor(config[key])
            cg.add(var.set_schedule_layer_text_sensor(layer, sens))

    if CONF_CONTROL_MODE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CONTROL_MODE])
        cg.add(var.set_control_mode_text_sensor(sens))

    if CONF_PUMP_RUN_STATE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PUMP_RUN_STATE])
        cg.add(var.set_pump_run_state_text_sensor(sens))

    if CONF_COMPONENT_BUILD in config:
        # Resolved here, not at runtime: the firmware has no git. Only when the
        # entity is configured, so no build pays for a subprocess it won't use.
        cg.add(var.set_build_revision(_component_source_revision()))
        sens = await text_sensor.new_text_sensor(config[CONF_COMPONENT_BUILD])
        cg.add(var.set_component_build_text_sensor(sens))

    if CONF_SCHEDULE_STALLED in config:
        sens = await binary_sensor.new_binary_sensor(config[CONF_SCHEDULE_STALLED])
        cg.add(var.set_schedule_stalled_binary_sensor(sens))

    if CONF_SERIAL_NUMBER in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SERIAL_NUMBER])
        cg.add(var.set_serial_number_text_sensor(sens))

    if CONF_SOFTWARE_VERSION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SOFTWARE_VERSION])
        cg.add(var.set_software_version_text_sensor(sens))

    if CONF_HARDWARE_VERSION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_HARDWARE_VERSION])
        cg.add(var.set_hardware_version_text_sensor(sens))

    if CONF_BLE_VERSION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_BLE_VERSION])
        cg.add(var.set_ble_version_text_sensor(sens))

    if CONF_PRODUCT_NAME in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PRODUCT_NAME])
        cg.add(var.set_product_name_text_sensor(sens))

    if CONF_PRODUCT_VERSION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PRODUCT_VERSION])
        cg.add(var.set_product_version_text_sensor(sens))

    if CONF_SINGLE_EVENTS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SINGLE_EVENTS])
        cg.add(var.set_single_events_text_sensor(sens))

    if CONF_VACATION in config:
        sens = await text_sensor.new_text_sensor(config[CONF_VACATION])
        cg.add(var.set_vacation_text_sensor(sens))

    if CONF_EVENT_LOG in config:
        sens = await text_sensor.new_text_sensor(config[CONF_EVENT_LOG])
        cg.add(var.set_event_log_text_sensor(sens))

    if CONF_HISTORY in config:
        sens = await text_sensor.new_text_sensor(config[CONF_HISTORY])
        cg.add(var.set_history_text_sensor(sens))

    if CONF_CYCLE_TIMESTAMPS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CYCLE_TIMESTAMPS])
        cg.add(var.set_cycle_timestamps_text_sensor(sens))

    if CONF_START_COUNT in config:
        sens = await sensor.new_sensor(config[CONF_START_COUNT])
        cg.add(var.set_start_count_sensor(sens))

    if CONF_OPERATING_HOURS in config:
        sens = await sensor.new_sensor(config[CONF_OPERATING_HOURS])
        cg.add(var.set_operating_hours_sensor(sens))

    if CONF_CLOCK_DIFF in config:
        sens = await sensor.new_sensor(config[CONF_CLOCK_DIFF])
        cg.add(var.set_clock_diff_sensor(sens))

    if CONF_LAST_CLOCK_SYNC in config:
        sens = await text_sensor.new_text_sensor(config[CONF_LAST_CLOCK_SYNC])
        cg.add(var.set_last_clock_sync_sensor(sens))

    if CONF_LINK_RECYCLES in config:
        sens = await sensor.new_sensor(config[CONF_LINK_RECYCLES])
        cg.add(var.set_link_recycles_sensor(sens))

    if CONF_LINK_MAX_GAP in config:
        sens = await sensor.new_sensor(config[CONF_LINK_MAX_GAP])
        cg.add(var.set_link_max_gap_sensor(sens))

    # One setter per rung, named after the threshold. Deliberately not an
    # index-based setter: the name is the only thing that tells an operator what
    # a counter means, and a name/index/threshold mismatch is invisible in a
    # reading. Resolving the setter by name makes drift a build failure.
    for threshold_s, key in zip(LINK_GAP_THRESHOLDS_S, CONF_LINK_GAPS_OVER, strict=True):
        if key in config:
            sens = await sensor.new_sensor(config[key])
            cg.add(getattr(var, f"set_link_gaps_over_{threshold_s}s_sensor")(sens))

    if CONF_LINK_GAPS_TRUNCATED in config:
        sens = await sensor.new_sensor(config[CONF_LINK_GAPS_TRUNCATED])
        cg.add(var.set_link_gaps_truncated_sensor(sens))

    if CONF_LINK_WATCH_TIME in config:
        sens = await sensor.new_sensor(config[CONF_LINK_WATCH_TIME])
        cg.add(var.set_link_watch_time_sensor(sens))

    if CONF_PUMP_LINK_STATUS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PUMP_LINK_STATUS])
        cg.add(var.set_pump_link_status_text_sensor(sens))

    if CONF_PUMP_LAST_LINK_FAILURE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PUMP_LAST_LINK_FAILURE])
        cg.add(var.set_pump_last_link_failure_text_sensor(sens))

    if CONF_PUMP_CLOCK_DST in config:
        sens = await text_sensor.new_text_sensor(config[CONF_PUMP_CLOCK_DST])
        cg.add(var.set_pump_clock_dst_text_sensor(sens))
    if CONF_FLOW_LIMITER in config:
        sens = await text_sensor.new_text_sensor(config[CONF_FLOW_LIMITER])
        cg.add(var.set_flow_limiter_text_sensor(sens))

    if CONF_FLOW_LIMITED in config:
        bsens = await binary_sensor.new_binary_sensor(config[CONF_FLOW_LIMITED])
        cg.add(var.set_flow_limiter_active_binary_sensor(bsens))

    # Set control state polling interval (fixes issue #54)
    if CONF_CONTROL_STATE_POLL_INTERVAL in config:
        cg.add(var.set_control_state_poll_interval(config[CONF_CONTROL_STATE_POLL_INTERVAL]))
