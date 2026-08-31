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

from . import CONF_FAN_RF_ID, CONF_KEY, FanRfDecoder, fan_rf_ns, validate_key

DEPENDENCIES = ["fan_rf"]

CONF_REPEATS = "repeats"

FanRfBinarySensor = fan_rf_ns.class_(
    "FanRfBinarySensor", binary_sensor.BinarySensor, cg.Component
)


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
