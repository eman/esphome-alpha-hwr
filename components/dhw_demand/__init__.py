"""ESPHome config schema for the DHW Demand Detector component."""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import binary_sensor, sensor, text_sensor
from esphome.const import (
    CONF_ID,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_SECOND,
)

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["sensor"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]

dhw_demand_ns = cg.esphome_ns.namespace("dhw_demand")
DhwDemandComponent = dhw_demand_ns.class_(
    "DhwDemandComponent", cg.PollingComponent
)

# ── Input sensor keys ────────────────────────────────────────────────────────
CONF_MOTOR_SPEED = "motor_speed"
CONF_MOTOR_CURRENT = "motor_current"
CONF_PUMP_FLOW = "pump_flow"
CONF_FLOW = "flow"
CONF_TANK_LOWER_TEMP = "tank_lower_temp"
CONF_DHW_CHARGE = "dhw_charge"
CONF_DHW_IN_USE = "dhw_in_use"

# ── Output sensor keys ───────────────────────────────────────────────────────
CONF_DEMAND = "demand"
CONF_CONFIDENCE = "confidence"
CONF_DEMAND_LEVEL = "demand_level"
CONF_SESSION_DURATION = "session_duration"
CONF_DETECTION_METHOD = "detection_method"

# ── Threshold keys ───────────────────────────────────────────────────────────
CONF_PUMP_OFF_CURRENT_THRESHOLD = "pump_off_current_threshold"
CONF_FLOW_THRESHOLD = "flow_threshold"
CONF_THERMAL_COLLAPSE_RATE = "thermal_collapse_rate"
CONF_DHW_CHARGE_DROP_RATE = "dhw_charge_drop_rate"
CONF_PUMP_ON_DEMAND_FLOW_THRESHOLD = "pump_on_demand_flow_threshold"
CONF_PUMP_ON_DEMAND_MIN_SPEED_RPM = "pump_on_demand_min_speed_rpm"
CONF_PUMP_ON_DEMAND_MAX_STALE_SECONDS = "pump_on_demand_max_stale_seconds"
CONF_FLOW_MAX_STALE_SECONDS = "flow_max_stale_seconds"
CONF_DHW_IN_USE_MIN_SECONDS = "dhw_in_use_min_seconds"
CONF_FLOW_LATCH_SECONDS = "flow_latch_seconds"
CONF_LATCH_PUMP_OFF_SUPPRESSION_SECONDS = "latch_pump_off_suppression_seconds"
CONF_PUMP_ON_SETTLE_SECONDS = "pump_on_demand_settle_seconds"
CONF_PUMP_ON_CONTINUATION_MAX_SECONDS = "pump_on_continuation_max_seconds"
CONF_SESSION_GAP_TOLERANCE_SECONDS = "session_gap_tolerance_seconds"
CONF_DEMAND_RELEASE_SECONDS = "demand_release_seconds"


# ── Retired keys ─────────────────────────────────────────────────────────────
# The pump-on hydraulic vote tier was replaced by a direct measurement in issue
# #149, and these keys configured it. They are kept in the schema so a config
# that still sets one fails with an explanation rather than a bare "[key] is an
# invalid option", which says nothing about what to do instead.
#
# Rejecting rather than ignoring is deliberate: config that validates but does
# nothing is a trap, and this fails while the person who typed it is looking.
_RETIRED_KEYS = {
    "inlet_pressure_transient_threshold": (
        "the pressure-transient vote fired on the pump's own speed changes"
    ),
    "inlet_pressure_demand_floor": (
        "a scalar floor on a quantity that moves with pump speed — no-draw "
        "inlet pressure runs 3.4 PSI at 1650 RPM to 13.0 at 3600, while a real "
        "draw reads 5.7 at 2400, so the populations overlap in the wrong "
        "direction and every candidate value fails somewhere"
    ),
    "pump_flow_collapse_threshold": (
        "another scalar on a speed-dependent quantity; the loop-flow reading "
        "it thresholded is now one term of the subtraction instead"
    ),
    "motor_current_spike_threshold": (
        "the current-spike vote fired on the pump's own speed changes — 73 % "
        "of its firings over 29 days fell within 25 s of a self-initiated "
        "speed change"
    ),
    "pump_power_spike_threshold": (
        "the power-spike vote had the same defect as the current vote it "
        "corroborated"
    ),
    "pump_head_rate": (
        "the head-rate vote was gated on at least one other vote having "
        "fired, so it had nothing left to ride on once the others retired"
    ),
    "pump_head_rate_threshold": (
        "the head-rate vote was gated on at least one other vote having "
        "fired, so it had nothing left to ride on once the others retired"
    ),
    "inlet_pressure": (
        "inlet pressure was read only by the two pressure votes"
    ),
    "pump_power": ("pump power was read only by the power-spike vote"),
}


def _retired(key):
    """Fail validation with what replaced this key and why it went."""

    def validator(value):
        raise cv.Invalid(
            f"'{key}' was removed in issue #149: {_RETIRED_KEYS[key]}. "
            "The pump-on branch no longer votes on hydraulics — it measures "
            "household demand directly as "
            "(household flow meter - pump loop flow), which scored precision "
            "0.808 against the vote tier's 0.530 on a controlled corpus, with "
            "0 pump-on false positives against 23. Delete this key. See "
            "pump_on_demand_flow_threshold, pump_on_demand_min_speed_rpm, "
            "pump_on_demand_max_stale_seconds and flow_max_stale_seconds "
            "for what is tunable now, and docs/configuration.md for the "
            "migration."
        )

    return validator


# ── Renamed keys ─────────────────────────────────────────────────────────────
# The staleness bound on the household flow channel was named after the meter
# one installation happened to use. The detector is sensor-agnostic, so the key
# now names the channel instead. Same rejection rule as the retired keys above:
# a config setting the old name fails rather than being silently ignored.
_RENAMED_KEYS = {
    "droplet_max_stale_seconds": "flow_max_stale_seconds",
}


def _renamed(old):
    """Fail validation naming the replacement key."""

    def validator(value):
        raise cv.Invalid(
            f"'{old}' was renamed to '{_RENAMED_KEYS[old]}'. The bound applies "
            "to the household flow channel regardless of which meter feeds it. "
            f"Behavior and default are unchanged — rename the key to "
            f"'{_RENAMED_KEYS[old]}'."
        )

    return validator


CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(DhwDemandComponent),

            # ── Outputs ──────────────────────────────────────────────────────
            cv.Optional(CONF_DEMAND): binary_sensor.binary_sensor_schema(),
            # Detection confidence, published as a percentage (0–100 %).
            cv.Optional(CONF_CONFIDENCE): sensor.sensor_schema(
                unit_of_measurement="%",
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            ),
            # Estimated draw intensity, 0.0–1.0. Part of the RFC-006 detector
            # contract; the Python detector publishes the same field.
            cv.Optional(CONF_DEMAND_LEVEL): sensor.sensor_schema(
                accuracy_decimals=2,
                state_class=STATE_CLASS_MEASUREMENT,
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:water-percent",
            ),
            cv.Optional(CONF_SESSION_DURATION): sensor.sensor_schema(
                unit_of_measurement=UNIT_SECOND,
                accuracy_decimals=0,
                state_class=STATE_CLASS_MEASUREMENT,
                icon="mdi:timer",
            ),
            cv.Optional(CONF_DETECTION_METHOD): text_sensor.text_sensor_schema(
                entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
                icon="mdi:information-outline",
            ),

            # ── Input sensor references (all optional) ───────────────────────
            cv.Optional(CONF_MOTOR_SPEED): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_MOTOR_CURRENT): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_PUMP_FLOW): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_FLOW): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_TANK_LOWER_TEMP): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DHW_CHARGE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DHW_IN_USE): cv.use_id(sensor.Sensor),

            # ── Detection thresholds ─────────────────────────────────────────
            cv.Optional(CONF_PUMP_OFF_CURRENT_THRESHOLD, default=0.03):
                cv.positive_float,
            cv.Optional(CONF_FLOW_THRESHOLD, default=0.3):
                cv.positive_float,
            cv.Optional(CONF_THERMAL_COLLAPSE_RATE, default=0.05):
                cv.positive_float,
            cv.Optional(CONF_DHW_CHARGE_DROP_RATE, default=0.005):
                cv.positive_float,
            cv.Optional(CONF_PUMP_ON_DEMAND_FLOW_THRESHOLD, default=0.3):
                cv.positive_float,
            # Below this speed the pump's own loop-flow estimate reads low and
            # the difference goes spuriously positive with the tap shut.
            cv.Optional(CONF_PUMP_ON_DEMAND_MIN_SPEED_RPM, default=1950.0):
                cv.positive_float,
            # Bounded rather than left open: the ms conversion is uint32_t and
            # wraps past ~49.7 days, and a wrapped bound would treat every
            # reading as fresh — the opposite of what a large value asks for.
            # An hour is far beyond any useful staleness bound.
            cv.Optional(CONF_PUMP_ON_DEMAND_MAX_STALE_SECONDS, default=30):
                cv.int_range(min=1, max=3600),
            cv.Optional(CONF_FLOW_MAX_STALE_SECONDS, default=60):
                cv.int_range(min=1, max=3600),
            # Bounded for the same uint32_t wrap reason as the staleness keys,
            # and 0 is legal and meaningful: "high right now is enough".
            cv.Optional(CONF_DHW_IN_USE_MIN_SECONDS, default=70):
                cv.int_range(min=0, max=3600),
            cv.Optional(CONF_FLOW_LATCH_SECONDS, default=30):
                cv.positive_int,
            # Disarms the latch above for this long after a pump-off edge, so
            # the latch cannot be armed by the pump's own collapsing loop flow.
            # Defaults to the latch's own reach: a shorter window would leave
            # readings the latch can still see, a longer one buys nothing.
            #
            # Bounded for the same uint32_t wrap reason as the staleness keys.
            # This one is multiplied by 1000 into a uint32_t millisecond window
            # at the call site, so an unbounded value wraps and yields a
            # nonsense window rather than a long one — `cv.positive_int` would
            # accept 999999999 and hand the component 3 567 586 328 ms.
            # 0 is legal and meaningful: it restores the previous behaviour.
            cv.Optional(CONF_LATCH_PUMP_OFF_SUPPRESSION_SECONDS, default=30):
                cv.int_range(min=0, max=3600),
            # Seconds after a pump start during which the subtraction declines,
            # because the pump's own loop-flow estimate is still spinning up and
            # reads low against a loop the meter already sees moving. Measured
            # across 296 starts: 0-10 s is p90 0.820 GPM with 26.6 % above the
            # 0.3 threshold, against a flat 6-9 % from 10 s out to 180 s.
            # Bounded for the uint32_t wrap reason; 0 disables.
            cv.Optional(CONF_PUMP_ON_SETTLE_SECONDS, default=10):
                cv.int_range(min=0, max=3600),
            # How long the continuation tier may keep asserting a draw that
            # nothing has measured since. It only ever runs out while the
            # subtraction is unavailable, because a subtraction below threshold
            # releases the tier on the spot; the case it bounds is a pump
            # turning below pump_on_demand_min_speed_rpm, where no measurement
            # of household draw exists at all and the claim would otherwise
            # stand for the whole run.
            #
            # Bounded for the same uint32_t wrap reason as the staleness keys.
            # 0 disables the tier outright rather than making it unbounded --
            # unbounded is the behaviour being removed.
            cv.Optional(CONF_PUMP_ON_CONTINUATION_MAX_SECONDS, default=600):
                cv.int_range(min=0, max=3600),
            cv.Optional(CONF_SESSION_GAP_TOLERANCE_SECONDS, default=60):
                cv.positive_int,
            cv.Optional(CONF_DEMAND_RELEASE_SECONDS, default=30):
                cv.positive_int,
            **{
                cv.Optional(key): _retired(key)
                for key in _RETIRED_KEYS
            },
            **{
                cv.Optional(old): _renamed(old)
                for old in _RENAMED_KEYS
            },
        }
    )
    .extend(cv.polling_component_schema("10s"))
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    # ── Outputs ───────────────────────────────────────────────────────────────
    if demand_config := config.get(CONF_DEMAND):
        bs = await binary_sensor.new_binary_sensor(demand_config)
        cg.add(var.set_demand_sensor(bs))

    if conf_config := config.get(CONF_CONFIDENCE):
        sens = await sensor.new_sensor(conf_config)
        cg.add(var.set_confidence_sensor(sens))

    if level_config := config.get(CONF_DEMAND_LEVEL):
        sens = await sensor.new_sensor(level_config)
        cg.add(var.set_demand_level_sensor(sens))

    if dur_config := config.get(CONF_SESSION_DURATION):
        sens = await sensor.new_sensor(dur_config)
        cg.add(var.set_session_duration_sensor(sens))

    if method_config := config.get(CONF_DETECTION_METHOD):
        ts = await text_sensor.new_text_sensor(method_config)
        cg.add(var.set_detection_method_sensor(ts))

    # ── Input sensor references ────────────────────────────────────────────────
    _input_map = {
        CONF_MOTOR_SPEED: "set_motor_speed_sensor",
        CONF_MOTOR_CURRENT: "set_motor_current_sensor",
        CONF_PUMP_FLOW: "set_pump_flow_sensor",
        CONF_FLOW: "set_flow_sensor",
        CONF_TANK_LOWER_TEMP: "set_tank_lower_temp_sensor",
        CONF_DHW_CHARGE: "set_dhw_charge_sensor",
        CONF_DHW_IN_USE: "set_dhw_in_use_sensor",
    }
    for conf_key, setter in _input_map.items():
        if sens_id := config.get(conf_key):
            sens = await cg.get_variable(sens_id)
            cg.add(getattr(var, setter)(sens))

    # ── Thresholds ─────────────────────────────────────────────────────────────
    cg.add(var.set_pump_off_current_threshold(
        config[CONF_PUMP_OFF_CURRENT_THRESHOLD]))
    cg.add(var.set_flow_threshold(
        config[CONF_FLOW_THRESHOLD]))
    cg.add(var.set_thermal_collapse_rate(
        config[CONF_THERMAL_COLLAPSE_RATE]))
    cg.add(var.set_dhw_charge_drop_rate(
        config[CONF_DHW_CHARGE_DROP_RATE]))
    cg.add(var.set_pump_on_demand_flow_threshold(
        config[CONF_PUMP_ON_DEMAND_FLOW_THRESHOLD]))
    cg.add(var.set_pump_on_demand_min_speed_rpm(
        config[CONF_PUMP_ON_DEMAND_MIN_SPEED_RPM]))
    cg.add(var.set_pump_on_demand_max_stale_seconds(
        config[CONF_PUMP_ON_DEMAND_MAX_STALE_SECONDS]))
    cg.add(var.set_flow_max_stale_seconds(
        config[CONF_FLOW_MAX_STALE_SECONDS]))
    cg.add(var.set_dhw_in_use_min_seconds(
        config[CONF_DHW_IN_USE_MIN_SECONDS]))
    cg.add(var.set_flow_latch_seconds(
        config[CONF_FLOW_LATCH_SECONDS]))
    cg.add(var.set_latch_pump_off_suppression_seconds(
        config[CONF_LATCH_PUMP_OFF_SUPPRESSION_SECONDS]))
    cg.add(var.set_pump_on_demand_settle_seconds(
        config[CONF_PUMP_ON_SETTLE_SECONDS]))
    cg.add(var.set_pump_on_continuation_max_seconds(
        config[CONF_PUMP_ON_CONTINUATION_MAX_SECONDS]))
    cg.add(var.set_session_gap_tolerance_seconds(
        config[CONF_SESSION_GAP_TOLERANCE_SECONDS]))
    cg.add(var.set_demand_release_seconds(
        config[CONF_DEMAND_RELEASE_SECONDS]))
