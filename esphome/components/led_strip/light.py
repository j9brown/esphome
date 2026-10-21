import esphome.codegen as cg
from esphome.components import esp32, light
import esphome.config_validation as cv
from esphome.const import CONF_COLOR_CORRECT, CONF_GAMMA_CORRECT, CONF_OUTPUT_ID
from esphome.types import ConfigType

from . import LedStrip, LutParams, get_led_strip_traits, get_or_create_lut, led_strip_ns

CODEOWNERS = ["@j9brown"]

LedStripLight = led_strip_ns.class_("LedStripLight", light.AddressableLight)

led_strip_renderer_ns = led_strip_ns.namespace("renderer")
ChannelOp = led_strip_renderer_ns.struct("ChannelOp")
ContinuousRenderer = led_strip_renderer_ns.class_("ContinuousRenderer")
DitherOpNone = led_strip_renderer_ns.struct("DitherOpNone")
DitherOpFast = led_strip_renderer_ns.struct("DitherOpFast")
ImmediateRenderer = led_strip_renderer_ns.class_("ImmediateRenderer")
InputOpFast = led_strip_renderer_ns.struct("InputOpFast")
InputOpLerp = led_strip_renderer_ns.struct("InputOpLerp")
Lut = led_strip_renderer_ns.struct("Lut")
LutOpNone = led_strip_renderer_ns.struct("LutOpNone")
LutOpFast = led_strip_renderer_ns.struct("LutOpFast")
LutOpLerp = led_strip_renderer_ns.struct("LutOpLerp")
OneBufferContinuousRenderer = led_strip_renderer_ns.class_(
    "OneBufferContinuousRenderer", ContinuousRenderer
)
Pipeline = led_strip_renderer_ns.struct("Pipeline")
Renderer = led_strip_renderer_ns.class_("Renderer")
RenderOpRGB = led_strip_renderer_ns.struct("RenderOpRGB")
RenderOpRGBCorrected = led_strip_renderer_ns.struct("RenderOpRGBCorrected")
RenderOpRGBW = led_strip_renderer_ns.struct("RenderOpRGBW")
RenderOpRGBWCorrected = led_strip_renderer_ns.struct("RenderOpRGBWCorrected")
TwoBufferContinuousRenderer = led_strip_renderer_ns.class_(
    "TwoBufferContinuousRenderer", ContinuousRenderer
)


CONF_DITHER_BITS = "dither_bits"
CONF_FRAME_RATE = "frame_rate"
CONF_INTERPOLATION = "interpolation"
CONF_LUT_ID = "lut_id"
CONF_LUT_METHOD = "lut_method"
CONF_LUT_MIN_OUTPUT = "lut_min_output"
CONF_LUT_MAX_OUTPUT = "lut_max_output"
CONF_LED_STRIP_ID = "led_strip_id"
CONF_PIPELINE_ID = "pipeline_id"
CONF_QUALITY = "quality"
CONF_RENDERER_ID = "renderer_id"

LUT_METHODS = {
    "fast": LutOpFast,
    "lerp": LutOpLerp,
}


def _validate_quality(config: ConfigType) -> ConfigType:
    if config[CONF_LUT_MAX_OUTPUT] < config[CONF_LUT_MIN_OUTPUT]:
        raise cv.Invalid("lux_max_output must be no less than lut_min_output")

    need_frame_rate = config[CONF_DITHER_BITS] != 0 or config[CONF_INTERPOLATION]
    if config.get(CONF_FRAME_RATE) is None:
        if need_frame_rate:
            raise cv.Invalid("Need frame rate when dither or interpolation is enabled")
    elif not need_frame_rate:
        raise cv.Invalid("Omit frame rate unless dither or interpolation is enabled")
    return config


CONFIG_SCHEMA = light.ADDRESSABLE_LIGHT_SCHEMA.extend(
    {
        cv.GenerateID(CONF_OUTPUT_ID): cv.declare_id(LedStripLight),
        cv.GenerateID(CONF_LED_STRIP_ID): cv.use_id(LedStrip),
        cv.GenerateID(CONF_LUT_ID): cv.declare_id(Lut),
        cv.GenerateID(CONF_PIPELINE_ID): cv.declare_id(Pipeline),
        cv.GenerateID(CONF_RENDERER_ID): cv.declare_id(Renderer),
        cv.Optional(CONF_QUALITY, default={}): cv.All(
            cv.Schema(
                {
                    cv.Optional(CONF_DITHER_BITS, default=0): cv.int_range(
                        min=0, max=4
                    ),
                    cv.Optional(CONF_INTERPOLATION, default=False): cv.boolean,
                    cv.Optional(CONF_FRAME_RATE): cv.All(
                        cv.framerate, cv.int_range(1, 1000)
                    ),
                    cv.Optional(CONF_LUT_METHOD, default="fast"): cv.enum(
                        LUT_METHODS, lower=True
                    ),
                    cv.Optional(CONF_LUT_MIN_OUTPUT, default=1): cv.int_range(
                        min=0, max=255
                    ),
                    cv.Optional(CONF_LUT_MAX_OUTPUT, default=255): cv.int_range(
                        min=0, max=255
                    ),
                }
            ),
            _validate_quality,
        ),
    }
).extend(cv.COMPONENT_SCHEMA)


def _final_validate(config: ConfigType) -> None:
    """Ensure that the light config is compatible with the referenced led output."""
    led_strip_id = config[CONF_LED_STRIP_ID]
    led_strip_traits = get_led_strip_traits(led_strip_id)

    if (color_correct := config.get(CONF_COLOR_CORRECT)) is not None:
        expected_num_channels = led_strip_traits.num_channels()
        actual_num_channels = len(color_correct)
        if actual_num_channels != expected_num_channels:
            raise cv.Invalid(
                f"Light color correction has {actual_num_channels} channels but should have {expected_num_channels} to match the associated LED Strip"
            )


FINAL_VALIDATE_SCHEMA = _final_validate


def _correction(scale: float) -> int:
    return int(round(scale * 256))


async def to_code(config: ConfigType) -> None:
    led_strip_id = config[CONF_LED_STRIP_ID]
    led_strip = await cg.get_variable(led_strip_id)
    led_strip_traits = get_led_strip_traits(led_strip_id)

    quality_config = config[CONF_QUALITY]

    input_op_type = InputOpLerp if quality_config[CONF_INTERPOLATION] else InputOpFast

    lut_params = LutParams()

    if (gamma_correct := config.get(CONF_GAMMA_CORRECT)) is not None:
        lut_params.gamma = gamma_correct
    lut_params.min_output = quality_config[CONF_LUT_MIN_OUTPUT]
    lut_params.max_output = quality_config[CONF_LUT_MAX_OUTPUT]

    if lut_params != LutParams():
        lut = get_or_create_lut(lut_params, config[CONF_LUT_ID])
        lut_op_type = quality_config[CONF_LUT_METHOD].enum_value
        lut_op = cg.StructInitializer(lut_op_type, ("lut", lut))
    else:
        lut_op_type = LutOpNone
        lut_op = cg.StructInitializer(lut_op_type)

    if (dither_bits := quality_config[CONF_DITHER_BITS]) > 0:
        dither_op_type = DitherOpFast
        dither_op = cg.StructInitializer(dither_op_type, ("dither_bits", dither_bits))
    else:
        dither_op_type = DitherOpNone
        dither_op = cg.StructInitializer(dither_op_type)

    channel_op_type = ChannelOp.template(lut_op_type, dither_op_type)
    channel_op = cg.StructInitializer(
        channel_op_type, ("lut_op", lut_op), ("dither_op", dither_op)
    )

    if led_strip_traits.has_w_channel():
        if (color_correct := config.get(CONF_COLOR_CORRECT)) is not None:
            render_op_type = RenderOpRGBWCorrected.template(channel_op_type)
            render_op = cg.StructInitializer(
                render_op_type,
                ("r_correction", _correction(color_correct[0])),
                ("g_correction", _correction(color_correct[1])),
                ("b_correction", _correction(color_correct[2])),
                ("w_correction", _correction(color_correct[3])),
                ("channel_op", channel_op),
            )
        else:
            render_op_type = RenderOpRGBW.template(channel_op_type)
            render_op = cg.StructInitializer(render_op_type, ("channel_op", channel_op))
    elif (color_correct := config.get(CONF_COLOR_CORRECT)) is not None:
        render_op_type = RenderOpRGBCorrected.template(channel_op_type)
        render_op = cg.StructInitializer(
            render_op_type,
            ("r_correction", _correction(color_correct[0])),
            ("g_correction", _correction(color_correct[1])),
            ("b_correction", _correction(color_correct[2])),
            ("channel_op", channel_op),
        )
    else:
        render_op_type = RenderOpRGB.template(channel_op_type)
        render_op = cg.StructInitializer(render_op_type, ("channel_op", channel_op))

    output_op_type = led_strip_traits.output_op_type

    pipeline = cg.new_Pvariable(
        config[CONF_PIPELINE_ID],
        cg.TemplateArguments(input_op_type, render_op_type, output_op_type),
        render_op,
    )

    if input_op_type.base == InputOpLerp.base:
        renderer_type = TwoBufferContinuousRenderer
    elif dither_op_type.base == DitherOpFast.base:
        renderer_type = OneBufferContinuousRenderer
    else:
        renderer_type = ImmediateRenderer

    if renderer_type.inherits_from(ContinuousRenderer):
        cg.add_define("USE_LED_STRIP_CONTINUOUS_RENDERER")
        esp32.include_builtin_idf_component("esp_timer")
        esp32.add_idf_sdkconfig_option(
            "CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD", True
        )
        renderer_args = [quality_config[CONF_FRAME_RATE]]
    else:
        renderer_args = []

    renderer = cg.Pvariable(
        config[CONF_RENDERER_ID], renderer_type.new(led_strip, *renderer_args, pipeline)
    )

    var = cg.new_Pvariable(config[CONF_OUTPUT_ID], renderer)

    filtered_light_config = {
        k: v
        for k, v in config.items()
        if k not in [CONF_COLOR_CORRECT, CONF_GAMMA_CORRECT]
    }
    await light.register_light(var, filtered_light_config)

    await cg.register_component(var, config)
