"""Guest thermostat: the ESPHome side. register_plugin() reads the id, version, tile memory and the screen's texts from
the manifest and translations/ beside this folder (docs/FIRMWARE_API.md). plugin.yaml gives the limits a guest may
set, the thermostat's own unit and the switch that says which unit the glass shows."""
import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.components import smart_display, switch
from esphome.const import CONF_ID

# The core component every Tessera screen has; the plugin API lives in it.
DEPENDENCIES = ["smart_display", "switch"]

CONF_FAHRENHEIT = "fahrenheit"
CONF_MIN_TEMP = "min_temp"
CONF_MAX_TEMP = "max_temp"
CONF_UNIT = "unit"

guest_thermostat_ns = cg.esphome_ns.namespace("guest_thermostat")
GuestThermostat = guest_thermostat_ns.class_("GuestThermostat", cg.Component)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(GuestThermostat),
        cv.Optional(CONF_FAHRENHEIT): cv.use_id(switch.Switch),
        cv.Optional(CONF_MIN_TEMP, default=7.0): cv.float_,
        cv.Optional(CONF_MAX_TEMP, default=25.0): cv.float_,
        cv.Optional(CONF_UNIT, default="C"): cv.one_of("C", "F", upper=True),
        cv.Optional("ignore_other_unit", default=False): cv.boolean,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    await smart_display.register_plugin(var, __file__)
    cg.add(var.set_limits(config[CONF_MIN_TEMP], config[CONF_MAX_TEMP]))
    cg.add(var.set_native_fahrenheit(config[CONF_UNIT] == "F"))
    cg.add(var.set_ignore_other_unit(config["ignore_other_unit"]))
    if CONF_FAHRENHEIT in config:
        cg.add(var.set_fahrenheit_switch(await cg.get_variable(config[CONF_FAHRENHEIT])))
