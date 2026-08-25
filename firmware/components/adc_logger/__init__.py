"""Continuous raw ADC streamer (diagnostic).

Streams every sample from one ADC pin to the log, so the pin can be plotted in
full and compared against an oscilloscope. No triggering: earlier triggered
versions kept deciding for us which windows we were allowed to see.
"""

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.const import CONF_CHANNEL, CONF_ID, CONF_SAMPLE_RATE

CONF_QUEUE_LINES = "queue_lines"

CODEOWNERS = ["@torval"]

smarterfan_ns = cg.global_ns.namespace("smarterfan")
AdcLogger = smarterfan_ns.class_("AdcLogger", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(AdcLogger),
        # ADC1 only (ADC2 is unusable while Wi-Fi runs): GPIO0-4 = channels 0-4.
        cv.Optional(CONF_CHANNEL, default=3): cv.int_range(min=0, max=4),
        # Every sample costs 3 log characters. ~10 kSa/s streams without
        # dropping; the C3's ADC ceiling of ~83 kHz is far beyond what the
        # logger can carry, so raising this trades detail for gaps.
        cv.Optional(CONF_SAMPLE_RATE, default=10000): cv.int_range(min=500, max=83000),
        # Elastic buffer, in 64-sample lines, absorbing logger stalls.
        cv.Optional(CONF_QUEUE_LINES, default=32): cv.int_range(min=4, max=256),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_channel(config[CONF_CHANNEL]))
    cg.add(var.set_sample_hz(config[CONF_SAMPLE_RATE]))
    cg.add(var.set_queue_lines(config[CONF_QUEUE_LINES]))
