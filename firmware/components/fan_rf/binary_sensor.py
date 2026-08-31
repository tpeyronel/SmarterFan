"""Per-button momentary sensors off the fan_rf decoder.

The `on_code` trigger is the useful primitive -- it carries the key, the press
counter, the raw codeword and the repeat index -- but a named binary_sensor per
button is what makes the remote show up in Home Assistant without a lambda.

Sensors register with the decoder rather than with `remote_receiver`, so a
capture is decoded once no matter how many buttons are configured.

By default a sensor also pulses on the repeats of a held button, one frame
period (41.1 ms) apart, which is what makes hold-to-dim possible from an
automation. Set `repeats: false` on a button where a hold should register only
once -- an on/off toggle, say.
"""

import esphome.codegen as cg
from esphome.components import binary_sensor
import esphome.config_validation as cv

from . import CONF_FAN_RF_ID, FanRfDecoder, fan_rf_ns

DEPENDENCIES = ["fan_rf"]

CONF_KEY = "key"
CONF_REPEATS = "repeats"

FanRfBinarySensor = fan_rf_ns.class_(
    "FanRfBinarySensor", binary_sensor.BinarySensor, cg.Component
)

# The remote's full key table, from PROTOCOL.md section 4. KEY is a lookup, not
# an encoding -- the values are not contiguous and nothing about a code predicts
# its button, so this table is transcribed, never computed. A raw 0-31 value is
# accepted too, for a button this remote does not have.
KEYS = {
    "bright_up": 3,
    "fan_forward": 4,
    "bright_down": 5,
    "all_off": 6,
    "colour_temp_up": 7,
    "light_toggle": 8,
    "timer_2h": 9,
    "fan_4": 10,
    "colour_temp_down": 11,
    "fan_6": 12,
    "cycle_full_bright": 13,
    "fan_5": 15,
    "fan_1": 16,
    "fan_reverse": 17,
    "fan_2": 18,
    "night_mode": 19,
    "natural_wind": 21,
    "fan_off": 22,
    "timer_4h": 25,
    "fan_3": 28,
}


def validate_key(value):
    if isinstance(value, str) and value.lower() in KEYS:
        return KEYS[value.lower()]
    return cv.int_range(min=0, max=31)(value)


CONFIG_SCHEMA = (
    binary_sensor.binary_sensor_schema(FanRfBinarySensor)
    .extend(
        {
            cv.GenerateID(CONF_FAN_RF_ID): cv.use_id(FanRfDecoder),
            cv.Required(CONF_KEY): validate_key,
            cv.Optional(CONF_REPEATS, default=True): cv.boolean,
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
)


async def to_code(config):
    var = await binary_sensor.new_binary_sensor(config)
    await cg.register_component(var, config)
    cg.add(var.set_key(config[CONF_KEY]))
    cg.add(var.set_repeats(config[CONF_REPEATS]))

    parent = await cg.get_variable(config[CONF_FAN_RF_ID])
    cg.add(parent.register_binary_sensor(var))
