import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import ble_client, sensor, binary_sensor, text_sensor
from esphome.const import (
    CONF_ID,
    DEVICE_CLASS_CONNECTIVITY,
    DEVICE_CLASS_CURRENT,
    DEVICE_CLASS_POWER,
    DEVICE_CLASS_TEMPERATURE,
    DEVICE_CLASS_VOLTAGE,
    ENTITY_CATEGORY_DIAGNOSTIC,
    STATE_CLASS_MEASUREMENT,
    UNIT_CELSIUS,
    UNIT_WATT,
)

CODEOWNERS = ["@eman"]
DEPENDENCIES = ["ble_client"]
AUTO_LOAD = ["binary_sensor", "sensor", "text_sensor"]

alpha_hwr_ns = cg.esphome_ns.namespace("alpha_hwr")
AlphaHwrComponent = alpha_hwr_ns.class_(
    "AlphaHwrComponent", cg.PollingComponent, ble_client.BLEClientNode
)

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
CONF_ENABLE_PAIRING = "enable_pairing"
CONF_ALARMS = "alarms"
CONF_WARNINGS = "warnings"
CONF_SCHEDULE = "schedule"
CONF_CONTROL_MODE = "control_mode"
CONF_SERIAL_NUMBER = "serial_number"
CONF_SOFTWARE_VERSION = "software_version"
CONF_HARDWARE_VERSION = "hardware_version"
CONF_BLE_VERSION = "ble_version"
CONF_PRODUCT_NAME = "product_name"
CONF_PRODUCT_VERSION = "product_version"
CONF_SINGLE_EVENTS = "single_events"
CONF_EVENT_LOG = "event_log"
CONF_HISTORY = "history"
CONF_CYCLE_TIMESTAMPS = "cycle_timestamps"
CONF_START_COUNT = "start_count"
CONF_OPERATING_HOURS = "operating_hours"
CONF_CLOCK_DIFF = "clock_diff"
CONF_LAST_CLOCK_SYNC = "last_clock_sync"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AlphaHwrComponent),
        cv.Required("ble_client_id"): cv.use_id(ble_client.BLEClient),
        cv.Optional(CONF_ENABLE_PAIRING, default=False): cv.boolean,
        cv.Optional(CONF_FLOW): sensor.sensor_schema(
            unit_of_measurement="m³/h",
            accuracy_decimals=3,
            device_class="volume_flow_rate",
            state_class=STATE_CLASS_MEASUREMENT,
        ),
        cv.Optional(CONF_HEAD): sensor.sensor_schema(
            unit_of_measurement="kPa",
            accuracy_decimals=2,
            device_class="pressure",
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
            unit_of_measurement="kPa/s",
            accuracy_decimals=3,
            state_class=STATE_CLASS_MEASUREMENT,
            entity_category=ENTITY_CATEGORY_DIAGNOSTIC,
            icon="mdi:gauge",
        ),
        cv.Optional(CONF_PAIRING_STATUS): binary_sensor.binary_sensor_schema(
            device_class=DEVICE_CLASS_CONNECTIVITY,
        ),
        cv.Optional(CONF_ALARMS): text_sensor.text_sensor_schema(
            icon="mdi:alert-circle",
        ),
        cv.Optional(CONF_WARNINGS): text_sensor.text_sensor_schema(
            icon="mdi:alert",
        ),
        cv.Optional(CONF_SCHEDULE): text_sensor.text_sensor_schema(
            icon="mdi:calendar-clock",
        ),
        cv.Optional(CONF_CONTROL_MODE): text_sensor.text_sensor_schema(
            icon="mdi:cog",
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
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    ble_client_var = await cg.get_variable(config["ble_client_id"])
    var = cg.new_Pvariable(config[CONF_ID], ble_client_var)
    await cg.register_component(var, config)

    # Set pairing enabled flag
    cg.add(var.set_pairing_enabled(config[CONF_ENABLE_PAIRING]))

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

    if CONF_ALARMS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_ALARMS])
        cg.add(var.set_alarms_text_sensor(sens))

    if CONF_WARNINGS in config:
        sens = await text_sensor.new_text_sensor(config[CONF_WARNINGS])
        cg.add(var.set_warnings_text_sensor(sens))

    if CONF_SCHEDULE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_SCHEDULE])
        cg.add(var.set_schedule_text_sensor(sens))

    if CONF_CONTROL_MODE in config:
        sens = await text_sensor.new_text_sensor(config[CONF_CONTROL_MODE])
        cg.add(var.set_control_mode_text_sensor(sens))

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
