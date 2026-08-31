"""Diagnostic dump of every decodable frame, as a 32-bit binary word.

`fan_rf` answers "which button was pressed"; this answers "what did the
receiver actually deliver". It applies exactly one test -- do the pulses match
the remote's two symbol widths -- and prints the frames that pass. Nothing is
validated, voted on or deduplicated, so what you read is what arrived.

Standalone: it needs `remote_receiver` and nothing else, so this directory can
be dropped into any project on its own. It is also where the frame decoder
lives for the whole repo -- `fan_rf` includes fan_rf_dump_protocol.h rather than
carrying a copy, so the symbol timings have exactly one definition.

Because of that, `fan_rf` AUTO_LOADs this component for the header alone.
MULTI_CONF_NO_DEFAULT below is what keeps that from creating a dumper nobody
asked for: no `fan_rf_dump:` block in the config means no instance, just the
source in the build. Add the block to get the frames printed; the two then run
side by side, since every listener sees every capture.
"""

import esphome.codegen as cg
from esphome.components import remote_base
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = ["@torval"]
DEPENDENCIES = ["remote_receiver"]
MULTI_CONF = True
# AUTO_LOAD from `fan_rf` must not conjure an instance -- see the docstring.
MULTI_CONF_NO_DEFAULT = True

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
