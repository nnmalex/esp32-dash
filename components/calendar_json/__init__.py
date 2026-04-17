import esphome.codegen as cg
import esphome.config_validation as cv

CODEOWNERS = ["@nnmalex"]

CONFIG_SCHEMA = cv.Schema({})


async def to_code(config):
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/calendar_json.h"')
    )
