import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import sensor, binary_sensor, number
from esphome.const import CONF_ID

DEPENDENCIES = ['esp32']
AUTO_LOAD = ['sensor', 'binary_sensor', 'number']
ns = cg.esphome_ns.namespace('buzz_detector')
BuzzDetector = ns.class_('BuzzDetector', cg.Component)
SENSORS = ['level', 'baseline', 'rise', 'streak', 'read_errors', 'overflows', 'report_drops']
BINARIES = ['detected', 'ready']
CONFIG_SCHEMA = cv.Schema({
    cv.GenerateID(): cv.declare_id(BuzzDetector),
    cv.Required('bclk_pin'): cv.int_range(min=0, max=27),
    cv.Required('ws_pin'): cv.int_range(min=0, max=27),
    cv.Required('data_pin'): cv.int_range(min=0, max=27),
    cv.Optional('capture_enabled', default=True): cv.boolean,
    cv.Optional('start_delay', default='5s'): cv.positive_time_period_milliseconds,
    cv.Optional('slot_index', default=0): cv.int_range(min=0, max=1),
    cv.Optional('report_interval', default='2s'): cv.positive_time_period_milliseconds,
    cv.Required('controls'): cv.All([cv.use_id(number.Number)], cv.Length(min=8, max=8)),
    **{cv.Required(k): cv.use_id(sensor.Sensor) for k in SENSORS},
    **{cv.Required(k): cv.use_id(binary_sensor.BinarySensor) for k in BINARIES},
}).extend(cv.COMPONENT_SCHEMA)

async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_pins(config['bclk_pin'], config['ws_pin'], config['data_pin']))
    cg.add(var.set_slot(config['slot_index']))
    cg.add(var.set_capture_enabled(config['capture_enabled']))
    cg.add(var.set_start_delay(config['start_delay'].total_milliseconds))
    cg.add(var.set_report_interval(config['report_interval'].total_milliseconds))
    for key in SENSORS + BINARIES:
        cg.add(getattr(var, 'set_' + key)(await cg.get_variable(config[key])))
    for i, control in enumerate(config['controls']):
        cg.add(var.set_control(i, await cg.get_variable(control)))
