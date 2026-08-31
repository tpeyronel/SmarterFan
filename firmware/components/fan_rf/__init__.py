"""Decoder for the Novohome NH-VTR500 433 MHz remote.

ESPHome's built-in `rc_switch` cannot decode this remote, and no amount of
tuning fixes it: RCSwitchBase::decode() calls expect_sync() once and then reads
bits from offset 0 of the capture. Every press sends the same 32-bit frame five
times, and the receiver's AGC damages the *first* one while it settles -- so the
built-in decoder always tries the one frame that is broken and never looks at
frames 2-5 sitting behind it, intact.

The whole pipeline lives here, in three stages:

    raw capture --> frames --> packets --> presses

Each stage can be watched from the console. `dump_frames` prints one line per
capture carrying the 32-bit codewords exactly as they came off the wire, before
any field is looked at -- that is the diagnostic that says what the receiver
actually delivered. `dump_commands` prints the decoded button presses. Both go
out at DEBUG. Under them sits `remote_receiver`'s own `dump: raw`, which prints
the pulse durations that feed stage one.

All three stages are in fan_rf_protocol.h, free of any framework dependency and
compiled unchanged into tools/fan_rf_selftest.cpp. The protocol itself is
documented in PROTOCOL.md.
"""

from esphome import automation
import esphome.codegen as cg
from esphome.components import remote_base
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TRIGGER_ID

CODEOWNERS = ["@torval"]
DEPENDENCIES = ["remote_receiver"]
MULTI_CONF = True

CONF_DUMP_COMMANDS = "dump_commands"
CONF_DUMP_FRAMES = "dump_frames"
CONF_FAN_RF_ID = "fan_rf_id"
CONF_ON_CODE = "on_code"
CONF_PRESS_FRAMES = "press_frames"
CONF_RUN_TIMEOUT = "run_timeout"

smarterfan_ns = cg.global_ns.namespace("smarterfan")
fan_rf_ns = smarterfan_ns.namespace("fan_rf")

FanRfDecoder = fan_rf_ns.class_(
    "FanRfDecoder", cg.Component, remote_base.RemoteReceiverListener
)
FanRfCodeTrigger = fan_rf_ns.class_(
    "FanRfCodeTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.uint32, cg.uint32),
)

CONFIG_SCHEMA = (
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(FanRfDecoder),
            # One press is five identical frames. Report the first, swallow the
            # rest, and treat every frame past them as a held repeat. Lower this
            # to make a hold start repeating sooner; set it to 1 to report every
            # frame the remote sends.
            cv.Optional(CONF_PRESS_FRAMES, default=5): cv.int_range(min=1, max=64),
            # A run of identical frames ends after this long without one. Frames
            # arrive every 41.1 ms while a button is held, so this is six frame
            # periods -- long enough to ride out dropped frames, short enough
            # that a release is noticed promptly. It also stops the ninth press
            # of one button (the counter wraps at 8, so the codeword repeats)
            # from being read as the same hold continuing.
            cv.Optional(
                CONF_RUN_TIMEOUT, default="250ms"
            ): cv.positive_time_period_milliseconds,
            # Stage 1 out: one line per capture, listing every 32-bit word
            # that decoded, before any field is inspected. Off by default --
            # it is the diagnostic for when something is wrong, and idle noise
            # produces captures constantly.
            cv.Optional(CONF_DUMP_FRAMES, default=False): cv.boolean,
            # Stage 3 out: the decoded button presses and held repeats.
            cv.Optional(CONF_DUMP_COMMANDS, default=True): cv.boolean,
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
    cg.add(var.set_press_frames(config[CONF_PRESS_FRAMES]))
    cg.add(var.set_run_timeout(config[CONF_RUN_TIMEOUT]))
    cg.add(var.set_dump_frames(config[CONF_DUMP_FRAMES]))
    cg.add(var.set_dump_commands(config[CONF_DUMP_COMMANDS]))

    for conf in config.get(CONF_ON_CODE, []):
        trigger = cg.new_Pvariable(conf[CONF_TRIGGER_ID], var)
        await automation.build_automation(
            trigger,
            [
                (cg.uint8, "key"),
                (cg.uint8, "counter"),
                (cg.uint32, "code"),
                (cg.uint32, "repeat"),
            ],
            conf,
        )
