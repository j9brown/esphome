import esphome.codegen as cg
from esphome.components import light
import esphome.config_validation as cv
from esphome.const import CONF_COLOR_CORRECT, CONF_GAMMA_CORRECT, CONF_OUTPUT_ID
from esphome.types import ConfigType

from . import (
    CONF_LED_STRIP_ID,
    LED_STRIP_CLIENT_SCHEMA,
    get_led_strip_traits,
    led_strip_ns,
    new_led_strip_renderer,
)

ColorScaleFloat = led_strip_ns.class_("ColorScaleFloat")
LedStripLight = led_strip_ns.class_("LedStripLight", light.AddressableLight)


CONFIG_SCHEMA = (
    light.ADDRESSABLE_LIGHT_SCHEMA.extend(
        {
            cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(LedStripLight),
            cv.Optional(CONF_GAMMA_CORRECT): cv.invalid(
                "'gamma_correct' has moved to 'renderer.lut.gamma_correct'"
            ),
            cv.Optional(light.CONF_POWER_ESTIMATION): cv.invalid(
                "'power_estimation' is not supported yet"
            ),
        }
    )
    .extend(LED_STRIP_CLIENT_SCHEMA)
    .extend(cv.COMPONENT_SCHEMA)
)


def _final_validate(config: ConfigType) -> None:
    """Ensure that the light config is compatible with the referenced led output."""
    led_strip_id = config[CONF_LED_STRIP_ID]
    led_strip_traits = get_led_strip_traits(led_strip_id)

    if (color_correct := config.get(CONF_COLOR_CORRECT)) is not None:
        expected_num_channels = led_strip_traits.num_channels
        actual_num_channels = len(color_correct)
        if actual_num_channels != expected_num_channels:
            raise cv.Invalid(
                f"'color_correct' has {actual_num_channels} channels but the LED strip has {expected_num_channels} channels"
            )
    else:
        config[CONF_COLOR_CORRECT] = [1.0] * led_strip_traits.num_channels


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    renderer_var = await new_led_strip_renderer(config)
    color_scale_expr = cg.ArrayInitializer(*config[CONF_COLOR_CORRECT])
    output_var = cg.new_Pvariable(
        config[CONF_OUTPUT_ID], renderer_var, color_scale_expr
    )

    # Disable the light platform's built-in color correction features by removing their keys.
    filtered_light_config = {
        k: v
        for k, v in config.items()
        if k not in [CONF_COLOR_CORRECT, CONF_GAMMA_CORRECT]
    }
    await light.register_light(output_var, filtered_light_config)
    await cg.register_component(output_var, config)
