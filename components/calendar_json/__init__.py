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
CONF_SETTINGS = "settings"
CONF_FIELDS = "fields"
CONF_GROUP = "group"
CONF_LABEL = "label"
CONF_HELP = "help"
CONF_BASE_URL = "base_url"
CONF_TOKEN = "token"
CONF_WEB_SERVER_BASE_ID = "web_server_base_id"
CONF_TITLE = "title"
CONF_USERNAME = "username"
CONF_PASSWORD = "password"
CONF_DOMAIN = "domain"
CONF_UNIT_OF_MEASUREMENT = "unit_of_measurement"
CONF_TEXT = "text"
CONF_NUMBER = "number"
CONF_SELECT = "select"
CONF_SWITCH = "switch"
CONF_DIAG_KEEP_BUILD = "diag_keep_build"

ENTITY_KINDS = (CONF_TEXT, CONF_NUMBER, CONF_SELECT, CONF_SWITCH)


def AUTO_LOAD(config):
    # The settings page needs the shared HTTP server; only pull it in when used.
    if config.get(CONF_SETTINGS):
        return ["web_server_base", "socket"]
    return []


esp32_dash_ns = cg.global_ns.namespace("esp32_dash")
HttpWorker = esp32_dash_ns.class_("HttpWorker", cg.Component)
SettingsPage = esp32_dash_ns.class_("SettingsPage", cg.Component)
SettingsKind = SettingsPage.enum("Kind", is_class=True)

_KIND_ENUM = {
    CONF_TEXT: SettingsKind.TEXT,
    CONF_NUMBER: SettingsKind.NUMBER,
    CONF_SELECT: SettingsKind.SELECT,
    CONF_SWITCH: SettingsKind.SWITCH,
}


def _entity_id_schema(kind):
    # Resolve the component classes lazily so calendar_json does not import
    # entity platforms a config does not use.
    module = __import__(f"esphome.components.{kind}", fromlist=["_"])
    cls = {
        CONF_TEXT: "Text",
        CONF_NUMBER: "Number",
        CONF_SELECT: "Select",
        CONF_SWITCH: "Switch",
    }[kind]
    return cv.use_id(getattr(module, cls))


def _field_schema(value):
    present = [k for k in ENTITY_KINDS if k in value] if isinstance(value, dict) else []
    if len(present) != 1:
        raise cv.Invalid(
            f"Each settings field needs exactly one of: {', '.join(ENTITY_KINDS)}"
        )
    kind = present[0]
    schema = cv.Schema(
        {
            cv.Required(kind): _entity_id_schema(kind),
            cv.Required(CONF_GROUP): cv.string,
            cv.Required(CONF_LABEL): cv.string,
            cv.Optional(CONF_HELP, default=""): cv.string,
            # text fields are HA entity pickers: the domains offered (empty = any)
            cv.Optional(CONF_DOMAIN, default=[]): cv.ensure_list(cv.string_strict),
            cv.Optional(CONF_UNIT_OF_MEASUREMENT, default=""): cv.string,
        }
    )
    return schema(value)


SETTINGS_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(SettingsPage),
        cv.GenerateID(CONF_WEB_SERVER_BASE_ID): cv.use_id(
            cg.esphome_ns.namespace("web_server_base").class_("WebServerBase")
        ),
        cv.Required(CONF_FETCHER): cv.use_id(HttpWorker),
        # Evaluated on the main loop at fetch time: ha_base_url changes at runtime.
        cv.Required(CONF_BASE_URL): cv.returning_lambda,
        cv.Required(CONF_TOKEN): cv.returning_lambda,
        cv.Optional(CONF_TITLE, default="ESP32 Dash"): cv.string,
        cv.Optional(CONF_USERNAME, default=""): cv.string,
        cv.Optional(CONF_PASSWORD, default=""): cv.sensitive(cv.string),
        cv.Required(CONF_FIELDS): cv.ensure_list(_field_schema),
    }
).extend(cv.COMPONENT_SCHEMA)


def _consume_settings_sockets(config):
    # Browser connections to the page (page load, state, entity polling and saves
    # can overlap). The listening socket is registered by web_server_base.
    from esphome.components import socket

    socket.consume_sockets(3, "calendar_json.settings")(config)
    return config

CONFIG_SCHEMA = cv.Schema(
    {
        # Diagnostics (alloc_diag.h): mark the running build valid at the start
        # of setup, so a build that crashes during boot is not rolled back and
        # reports its own crash. Off unless a boot crash is being diagnosed.
        cv.Optional(CONF_DIAG_KEEP_BUILD, default=False): cv.boolean,
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
        # Device-hosted settings page (see settings_page.h).
        cv.Optional(CONF_SETTINGS): cv.All(SETTINGS_SCHEMA, _consume_settings_sockets),
    }
)


async def to_code(config):
    # Emitted into setup() at this component's (default, COMPONENT) priority:
    # after safe_mode's boot-loop check (APPLICATION), so safe mode still counts
    # a boot that then crashes, but before any component's setup() runs.
    cg.add(cg.RawExpression("esp32_dash::alloc_diag::install()"))
    if config[CONF_DIAG_KEEP_BUILD]:
        cg.add(cg.RawExpression("esp32_dash::alloc_diag::keep_this_build()"))
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/calendar_json.h"')
    )
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/weather_icons.h"')
    )
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/http_worker.h"')
    )
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/alloc_diag.h"')
    )

    if fetcher := config.get(CONF_FETCHER):
        var = cg.new_Pvariable(fetcher[CONF_ID])
        await cg.register_component(var, fetcher)
        cg.add(var.set_verify_ssl(fetcher[CONF_VERIFY_SSL]))
        cg.add(var.set_timeout(fetcher[CONF_TIMEOUT]))
        esp32.include_builtin_idf_component("esp_http_client")

    if settings := config.get(CONF_SETTINGS):
        await _settings_to_code(settings)


async def _settings_to_code(config):
    cg.add_define("USE_ESP32_DASH_SETTINGS")
    cg.add_global(
        cg.RawStatement('#include "esphome/components/calendar_json/settings_page.h"')
    )
    base = await cg.get_variable(config[CONF_WEB_SERVER_BASE_ID])
    var = cg.new_Pvariable(config[CONF_ID], base)
    await cg.register_component(var, config)
    cg.add(var.set_fetcher(await cg.get_variable(config[CONF_FETCHER])))
    cg.add(
        var.set_base_url(
            await cg.process_lambda(config[CONF_BASE_URL], [], return_type=cg.std_string)
        )
    )
    cg.add(
        var.set_token(
            await cg.process_lambda(config[CONF_TOKEN], [], return_type=cg.std_string)
        )
    )
    cg.add(var.set_title(config[CONF_TITLE]))
    cg.add(var.set_credentials(config[CONF_USERNAME], config[CONF_PASSWORD]))

    for field in config[CONF_FIELDS]:
        kind = next(k for k in ENTITY_KINDS if k in field)
        entity = await cg.get_variable(field[kind])
        cg.add(
            var.add_field(
                str(field[kind].id),
                field[CONF_GROUP],
                field[CONF_LABEL],
                field[CONF_HELP],
                ",".join(field[CONF_DOMAIN]),
                field[CONF_UNIT_OF_MEASUREMENT],
                _KIND_ENUM[kind],
                cg.RawExpression(f"static_cast<void *>({entity})"),
            )
        )
