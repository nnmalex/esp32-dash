import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_TIMEOUT

CODEOWNERS = ["@nnmalex"]

# http_request configures the TLS sdkconfig the fetcher shares (certificate
# bundle, or insecure mode when verify_ssl is false), so keep the two
# verify_ssl settings in step.
DEPENDENCIES = ["http_request"]

CONF_FETCHER = "fetcher"
CONF_VERIFY_SSL = "verify_ssl"

esp32_dash_ns = cg.global_ns.namespace("esp32_dash")
HttpWorker = esp32_dash_ns.class_("HttpWorker", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        # Background HTTP worker for the calendar and forecast fetches: runs
        # requests off the main loop and calls back on it (see http_worker.h).
        cv.Optional(CONF_FETCHER): cv.Schema(
            {
                cv.GenerateID(): cv.declare_id(HttpWorker),
                cv.Optional(CONF_VERIFY_SSL, default=True): cv.boolean,
                # Per socket operation. Generous because a slow fetch no longer
                # blocks anything: most of a calendar fetch is HA waiting on the
                # upstream provider before it sends headers.
                cv.Optional(
                    CONF_TIMEOUT, default="20s"
                ): cv.positive_time_period_milliseconds,
            }
        ).extend(cv.COMPONENT_SCHEMA),
    }
)


async def to_code(config):
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/calendar_json.h"')
    )
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/weather_icons.h"')
    )
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/http_worker.h"')
    )

    if fetcher := config.get(CONF_FETCHER):
        var = cg.new_Pvariable(fetcher[CONF_ID])
        await cg.register_component(var, fetcher)
        cg.add(var.set_verify_ssl(fetcher[CONF_VERIFY_SSL]))
        cg.add(var.set_timeout(fetcher[CONF_TIMEOUT]))
        esp32.include_builtin_idf_component("esp_http_client")
