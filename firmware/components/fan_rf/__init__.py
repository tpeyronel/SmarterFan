"""Decoder for the Novohome NH-VTR500 433 MHz remote.

ESPHome's built-in `rc_switch` cannot decode this remote, and no amount of
tuning fixes it: RCSwitchBase::decode() calls expect_sync() once and then reads
bits from offset 0 of the capture. Every press sends the same 32-bit frame five
times, and the receiver's AGC damages the *first* one while it settles -- so the
built-in decoder always tries the one frame that is broken and never looks at
frames 2-5 sitting behind it, intact.

This component splits a capture on the 8.79 ms inter-frame gap, decodes every
frame independently, majority-votes the survivors, validates the device ID and
check nibble, and deduplicates on the press counter so one press yields one
event. The protocol itself lives in fan_rf_protocol.h, which is free of any
framework dependency and is compiled unchanged into tools/fan_rf_selftest.cpp.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import remote_base
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@torval"]
DEPENDENCIES = ["remote_receiver"]
MULTI_CONF = True

CONF_DEDUP_WINDOW = "dedup_window"
CONF_FAN_RF_ID = "fan_rf_id"
CONF_ON_CODE = "on_code"

smarterfan_ns = cg.global_ns.namespace("smarterfan")
fan_rf_ns = smarterfan_ns.namespace("fan_rf")

FanRfDecoder = fan_rf_ns.class_(
    "FanRfDecoder", cg.Component, remote_base.RemoteReceiverListener
)
FanRfCodeTrigger = fan_rf_ns.class_(
    "FanRfCodeTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.uint32),
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FanRfDecoder),
            # A press is five repeats carrying one counter value, so an
            # identical codeword arriving again inside this window is the same
            # press -- a burst split across two captures, or a held button.
            # Consecutive presses always differ in counter, and it would take
            # eight presses inside the window to alias back onto one.
            cv.Optional(
                CONF_DEDUP_WINDOW, default="1000ms"
            ): cv.positive_time_period_milliseconds,
            cv.Optional(CONF_ON_CODE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FanRfCodeTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
    cg.add(var.set_dedup_window(config[CONF_DEDUP_WINDOW]))

    for conf in config.get(CONF_ON_CODE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [(cg.uint8, "command"), (cg.uint8, "counter"), (cg.uint32, "code")],
            conf,
        )
