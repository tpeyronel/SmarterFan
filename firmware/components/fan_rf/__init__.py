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

Two more stages sit on top for the transmit path -- the waveform builders and
the tap-versus-hold relay decision -- and they live in the same header for the
same reason.

All of it is in fan_rf_protocol.h, free of any framework dependency and compiled
unchanged into tools/fan_rf_selftest.cpp. The protocol itself is documented in
PROTOCOL.md.
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
CONF_KEY = "key"
CONF_ON_CODE = "on_code"
CONF_PRESS_FRAMES = "press_frames"
CONF_RELAY = "relay"
CONF_RELAY_DECISION = "relay_decision"
CONF_RUN_TIMEOUT = "run_timeout"
CONF_TAP_FRAMES = "tap_frames"

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


smarterfan_ns = cg.global_ns.namespace("smarterfan")
fan_rf_ns = smarterfan_ns.namespace("fan_rf")

FanRfDecoder = fan_rf_ns.class_(
    "FanRfDecoder", cg.Component, remote_base.RemoteReceiverListener
)
FanRfCodeTrigger = fan_rf_ns.class_(
    "FanRfCodeTrigger",
    automation.Trigger.template(cg.uint8, cg.uint8, cg.uint32, cg.uint32),
)
FanRfSendKeyAction = fan_rf_ns.class_("FanRfSendKeyAction", automation.Action)

RelayMode = fan_rf_ns.enum("RelayMode")
RELAY_MODES = {
    # Decode only. The transmitter stays configured and `fan_rf.send_key` still
    # works -- this is the bring-up setting, not a way to disable the component.
    "none": RelayMode.RELAY_NONE,
    # Fan buttons only, plus the five whose side has never been verified. What
    # this becomes once the ESP32 drives the LEDs itself and the light keys stop
    # needing to reach the OEM MCU.
    "fan": RelayMode.RELAY_FAN,
    "all": RelayMode.RELAY_ALL,
}


def _validate_relay(config):
    """Tie the relay default to whether there is anywhere to relay to.

    A receive-only build has no transmitter and must not ask for one: without
    this it would default to `all` and warn on every press it could not inject.
    """
    if remote_base.CONF_TRANSMITTER_ID in config:
        config.setdefault(CONF_RELAY, "all")
    elif config.get(CONF_RELAY, "none") != "none":
        raise cv.Invalid(
            f"'{CONF_RELAY}' needs a '{remote_base.CONF_TRANSMITTER_ID}' to relay to",
            path=[CONF_RELAY],
        )
    else:
        config[CONF_RELAY] = "none"
    return config


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
            # Which buttons are forwarded to the OEM MCU. Defaults to `all`
            # when a transmitter is configured -- the light keys stop being
            # relayed only once the ESP32 drives the LEDs itself, which does not
            # exist yet -- and to `none` when there is not one. See
            # _validate_relay below.
            cv.Optional(CONF_RELAY): cv.one_of(*RELAY_MODES, lower=True),
            # How long a press waits before it is called a tap.
            #
            # Nothing is injected on the press event: the point of waiting is
            # that a repeat arriving first says the button is held, and only
            # then does the relay know whether it is sending one frame or a
            # stream. The tracker swallows `press_frames` frames, so the first
            # repeat lands press_frames x 41.1 ms = 205 ms after the press --
            # this has to sit past that, and every millisecond of it is added
            # latency on every tap.
            #
            # 230 ms is 205 plus jitter. It deliberately does not carry a whole
            # extra frame period of margin for a dropped sixth frame: if that
            # frame is lost the deadline fires and one frame goes out, and
            # because a tap and the first frame of a hold are the same frame on
            # the same counter, the repeat behind it simply extends the stream.
            #
            # Lower `press_frames` to shorten both windows together.
            cv.Optional(
                CONF_RELAY_DECISION, default="230ms"
            ): cv.positive_time_period_milliseconds,
            # Frames one injected tap sends. The remote sends five, but that
            # redundancy buys margin on a noisy RF link and this goes over a
            # wire. UNVERIFIED -- nobody has asked the MCU whether one is
            # enough. Raise it if one turns out not to be.
            cv.Optional(CONF_TAP_FRAMES, default=1): cv.int_range(min=1, max=8),
            cv.Optional(CONF_ON_CODE): automation.validate_automation(
                {cv.GenerateID(CONF_TRIGGER_ID): cv.declare_id(FanRfCodeTrigger)}
            ),
        }
    )
    .extend(cv.COMPONENT_SCHEMA)
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
    # Optional: a receive-only build has no transmitter, and sniffer.yaml is one.
    .extend(
        cv.Schema(
            {
                cv.Optional(remote_base.CONF_TRANSMITTER_ID): cv.use_id(
                    remote_base.RemoteTransmitterBase
                )
            }
        )
    )
    .add_extra(_validate_relay)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
    cg.add(var.set_press_frames(config[CONF_PRESS_FRAMES]))
    cg.add(var.set_run_timeout(config[CONF_RUN_TIMEOUT]))
    cg.add(var.set_dump_frames(config[CONF_DUMP_FRAMES]))
    cg.add(var.set_dump_commands(config[CONF_DUMP_COMMANDS]))
    cg.add(var.set_relay_mode(RELAY_MODES[config[CONF_RELAY]]))
    cg.add(var.set_relay_decision(config[CONF_RELAY_DECISION]))
    cg.add(var.set_tap_frames(config[CONF_TAP_FRAMES]))

    if remote_base.CONF_TRANSMITTER_ID in config:
        await remote_base.register_transmittable(var, config)

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


# fan_rf.send_key: one press, from Home Assistant or any other automation, on
# the ESP32's own press counter. Holds are not supported in this pass.
SEND_KEY_ACTION_SCHEMA = cv.maybe_simple_value(
    {
        cv.GenerateID(CONF_FAN_RF_ID): cv.use_id(FanRfDecoder),
        cv.Required(CONF_KEY): cv.templatable(validate_key),
    },
    key=CONF_KEY,
)


@automation.register_action(
    "fan_rf.send_key", FanRfSendKeyAction, SEND_KEY_ACTION_SCHEMA, synchronous=True
)
async def send_key_action_to_code(config, action_id, template_arg, args):
    var = cg.new_Pvariable(action_id, template_arg)
    await cg.register_parented(var, config[CONF_FAN_RF_ID])
    cg.add(var.set_key(await cg.templatable(config[CONF_KEY], args, cg.uint8)))
    return var
