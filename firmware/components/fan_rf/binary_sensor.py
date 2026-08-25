"""Per-command momentary buttons off the fan_rf decoder.

The `on_code` trigger is the useful primitive -- it carries the command byte
and the press counter -- but a named binary_sensor per button is what makes the
remote show up in Home Assistant without a lambda.

Sensors register with the decoder rather than with `remote_receiver`, so a
capture is decoded once no matter how many buttons are configured.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv
from esphome.const import CONF_COMMAND

from . import CONF_FAN_RF_ID, FanRfDecoder, fan_rf_ns

DEPENDENCIES = ["fan_rf"]

FanRfBinarySensor = fan_rf_ns.class_(
    "FanRfBinarySensor", binary_sensor.BinarySensor, cg.Component
)

# Only three of the remote's buttons have been captured. Unknown command bytes
# are accepted as raw numbers rather than rejected -- the device ID and the
# check nibble are what validate a packet, not the command.
COMMANDS = {
    "brightness_up": 0x21,
    "colour_temp_up": 0x23,
    "fan_speed_1": 0x28,
}


def validate_command(value):
    if isinstance(value, str) and value.lower() in COMMANDS:
        return COMMANDS[value.lower()]
    return cv.hex_uint8_t(value)


CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(FanRfBinarySensor)
    .extend(
        {
            cv.GenerateID(CONF_FAN_RF_ID): cv.use_id(FanRfDecoder),
            cv.Required(CONF_COMMAND): validate_command,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_command(config[CONF_COMMAND]))

    parent = await cg.get_variable(config[CONF_FAN_RF_ID])
    cg.add(parent.register_binary_sensor(var))
