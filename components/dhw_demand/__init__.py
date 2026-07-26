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
CONF_INLET_PRESSURE = "inlet_pressure"
CONF_PUMP_FLOW = "pump_flow"
CONF_PUMP_POWER = "pump_power"
CONF_FLOW = "flow"
CONF_TANK_LOWER_TEMP = "tank_lower_temp"
CONF_DHW_CHARGE = "dhw_charge"
CONF_DHW_IN_USE = "dhw_in_use"

# ── Output sensor keys ───────────────────────────────────────────────────────
CONF_DEMAND = "demand"
CONF_CONFIDENCE = "confidence"
CONF_SESSION_DURATION = "session_duration"
CONF_DETECTION_METHOD = "detection_method"

# ── Threshold keys ───────────────────────────────────────────────────────────
CONF_PUMP_OFF_CURRENT_THRESHOLD = "pump_off_current_threshold"
CONF_FLOW_THRESHOLD = "flow_threshold"
CONF_THERMAL_COLLAPSE_RATE = "thermal_collapse_rate"
CONF_DHW_CHARGE_DROP_RATE = "dhw_charge_drop_rate"
CONF_INLET_PRESSURE_TRANSIENT_THRESHOLD = "inlet_pressure_transient_threshold"
CONF_INLET_PRESSURE_DEMAND_FLOOR = "inlet_pressure_demand_floor"
CONF_PUMP_FLOW_COLLAPSE_THRESHOLD = "pump_flow_collapse_threshold"
CONF_MOTOR_CURRENT_SPIKE_THRESHOLD = "motor_current_spike_threshold"
CONF_PUMP_POWER_SPIKE_THRESHOLD = "pump_power_spike_threshold"
CONF_PUMP_HEAD_RATE = "pump_head_rate"
CONF_PUMP_HEAD_RATE_THRESHOLD = "pump_head_rate_threshold"
CONF_FLOW_LATCH_SECONDS = "flow_latch_seconds"
CONF_SESSION_GAP_TOLERANCE_SECONDS = "session_gap_tolerance_seconds"
CONF_DEMAND_RELEASE_SECONDS = "demand_release_seconds"

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
            cv.Optional(CONF_INLET_PRESSURE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_PUMP_FLOW): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_PUMP_POWER): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_FLOW): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_TANK_LOWER_TEMP): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DHW_CHARGE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_DHW_IN_USE): cv.use_id(sensor.Sensor),
            cv.Optional(CONF_PUMP_HEAD_RATE): cv.use_id(sensor.Sensor),

            # ── Detection thresholds ─────────────────────────────────────────
            cv.Optional(CONF_PUMP_OFF_CURRENT_THRESHOLD, default=0.03):
                cv.positive_float,
            cv.Optional(CONF_FLOW_THRESHOLD, default=0.3):
                cv.positive_float,
            cv.Optional(CONF_THERMAL_COLLAPSE_RATE, default=0.05):
                cv.positive_float,
            cv.Optional(CONF_DHW_CHARGE_DROP_RATE, default=0.005):
                cv.positive_float,
            cv.Optional(CONF_INLET_PRESSURE_TRANSIENT_THRESHOLD, default=0.07):
                cv.positive_float,
            cv.Optional(CONF_INLET_PRESSURE_DEMAND_FLOOR, default=5.0):
                cv.positive_float,
            cv.Optional(CONF_PUMP_FLOW_COLLAPSE_THRESHOLD, default=0.2):
                cv.positive_float,
            cv.Optional(CONF_MOTOR_CURRENT_SPIKE_THRESHOLD, default=0.001):
                cv.positive_float,
            cv.Optional(CONF_PUMP_POWER_SPIKE_THRESHOLD, default=5.0):
                cv.positive_float,
            # m/s. Head telemetry is meters of head; 0.31 m/s ≈ the former
            # 3.0 kPa/s threshold (÷9.80665).
            cv.Optional(CONF_PUMP_HEAD_RATE_THRESHOLD, default=0.31):
                cv.positive_float,
            cv.Optional(CONF_FLOW_LATCH_SECONDS, default=30):
                cv.positive_int,
            cv.Optional(CONF_SESSION_GAP_TOLERANCE_SECONDS, default=60):
                cv.positive_int,
            cv.Optional(CONF_DEMAND_RELEASE_SECONDS, default=30):
                cv.positive_int,
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
        CONF_INLET_PRESSURE: "set_inlet_pressure_sensor",
        CONF_PUMP_FLOW: "set_pump_flow_sensor",
        CONF_PUMP_POWER: "set_pump_power_sensor",
        CONF_FLOW: "set_flow_sensor",
        CONF_TANK_LOWER_TEMP: "set_tank_lower_temp_sensor",
        CONF_DHW_CHARGE: "set_dhw_charge_sensor",
        CONF_DHW_IN_USE: "set_dhw_in_use_sensor",
        CONF_PUMP_HEAD_RATE: "set_pump_head_rate_sensor",
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
    cg.add(var.set_inlet_pressure_transient_threshold(
        config[CONF_INLET_PRESSURE_TRANSIENT_THRESHOLD]))
    cg.add(var.set_inlet_pressure_demand_floor(
        config[CONF_INLET_PRESSURE_DEMAND_FLOOR]))
    cg.add(var.set_pump_flow_collapse_threshold(
        config[CONF_PUMP_FLOW_COLLAPSE_THRESHOLD]))
    cg.add(var.set_motor_current_spike_threshold(
        config[CONF_MOTOR_CURRENT_SPIKE_THRESHOLD]))
    cg.add(var.set_pump_power_spike_threshold(
        config[CONF_PUMP_POWER_SPIKE_THRESHOLD]))
    cg.add(var.set_pump_head_rate_threshold(
        config[CONF_PUMP_HEAD_RATE_THRESHOLD]))
    cg.add(var.set_flow_latch_seconds(
        config[CONF_FLOW_LATCH_SECONDS]))
    cg.add(var.set_session_gap_tolerance_seconds(
        config[CONF_SESSION_GAP_TOLERANCE_SECONDS]))
    cg.add(var.set_demand_release_seconds(
        config[CONF_DEMAND_RELEASE_SECONDS]))
