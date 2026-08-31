"""Diagnostic dump of every decodable frame, as a 32-bit binary word.

`fan_rf` answers "which button was pressed"; this answers "what did the
receiver actually deliver". It applies exactly one test -- do the pulses match
the remote's two symbol widths -- and prints the frames that pass. Nothing is
validated, voted on or deduplicated, so what you read is what arrived.

Standalone: it needs `remote_receiver` and nothing else. Its frame decoder is
its own, in fan_rf_dump_protocol.h, so this directory can be dropped into any
project on its own. The trade is that the symbol timings also exist in
`fan_rf` -- re-measure the remote and both have to change.

It can run alongside `fan_rf`: every listener sees every capture, so running
both costs nothing but the decode.
"""

import esphome.codegen as cg
from esphome.components import remote_base
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@torval"]
DEPENDENCIES = ["remote_receiver"]
MULTI_CONF = True

smarterfan_ns = cg.global_ns.namespace("smarterfan")
fan_rf_dump_ns = smarterfan_ns.namespace("fan_rf_dump")

FanRfDump = fan_rf_dump_ns.class_(
    "FanRfDump", cg.Component, remote_base.RemoteReceiverListener
)

CONFIG_SCHEMA = (
    cv.Schema({cv.GenerateID(): cv.declare_id(FanRfDump)})
    .extend(cv.COMPONENT_SCHEMA)
    .extend(remote_base.REMOTE_LISTENER_SCHEMA)
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await remote_base.register_listener(var, config)
