from dataclasses import dataclass, field
import logging

import esphome.codegen as cg
from esphome.components import esp32
import esphome.config_validation as cv
from esphome.const import (
    CONF_DITHER,
    CONF_GAMMA_CORRECT,
    CONF_INPUT,
    CONF_MAX_LEVEL,
    CONF_METHOD,
    CONF_MIN_LEVEL,
)
from esphome.core import CORE, ID, HexInt
from esphome.types import ConfigType

_LOGGER = logging.getLogger(__name__)

CODEOWNERS = ["@j9brown"]
IS_PLATFORM_COMPONENT = True
DOMAIN = "led_strip"

led_strip_ns = cg.esphome_ns.namespace("led_strip")
LedStrip = led_strip_ns.class_("LedStrip")

led_strip_renderer_ns = led_strip_ns.namespace("renderer")
ChannelOp = led_strip_renderer_ns.struct("ChannelOp")
ContinuousRenderer = led_strip_renderer_ns.class_("ContinuousRenderer")
FastContinuousRenderer = led_strip_renderer_ns.class_(
    "FastContinuousRenderer", ContinuousRenderer
)
DitherOpNone = led_strip_renderer_ns.struct("DitherOpNone")
DitherOpSpatial = led_strip_renderer_ns.struct("DitherOpSpatial")
DitherOpTemporal = led_strip_renderer_ns.struct("DitherOpTemporal")
ImmediateRenderer = led_strip_renderer_ns.class_("ImmediateRenderer")
InputOpFast = led_strip_renderer_ns.struct("InputOpFast")
InputOpLerp = led_strip_renderer_ns.struct("InputOpLerp")
LerpContinuousRenderer = led_strip_renderer_ns.class_(
    "LerpContinuousRenderer", ContinuousRenderer
)
Lut = led_strip_renderer_ns.struct("Lut")
LutOpNone = led_strip_renderer_ns.struct("LutOpNone")
LutOpFast = led_strip_renderer_ns.struct("LutOpFast")
LutOpLerp = led_strip_renderer_ns.struct("LutOpLerp")
Pipeline = led_strip_renderer_ns.struct("Pipeline")
Renderer = led_strip_renderer_ns.class_("Renderer")
RenderOpRGB = led_strip_renderer_ns.struct("RenderOpRGB")
RenderOpRGBW = led_strip_renderer_ns.struct("RenderOpRGBW")


CONF_BITS = "bits"
CONF_FRAME_RATE = "frame_rate"
CONF_FRAME_STATS = "frame_stats"
CONF_LUT = "lut"
CONF_LUT_ID = "lut_id"
CONF_LED_STRIP_ID = "led_strip_id"
CONF_PIPELINE_ID = "pipeline_id"
CONF_RENDERER = "renderer"
CONF_RENDERER_ID = "renderer_id"

INPUT_METHODS = {
    "fast": InputOpFast,
    "lerp": InputOpLerp,
}

DITHER_METHODS = {
    "none": DitherOpNone,
    "spatial": DitherOpSpatial,
    "temporal": DitherOpTemporal,
}

LUT_METHODS = {
    "none": LutOpNone,
    "fast": LutOpFast,
    "lerp": LutOpLerp,
}

# Automatic frame rate range based on the perceptual limits of the human visual system.
# Lower frame rates may yield more pronounced flickering artifacts.
# Higher frame rates may waste resources for little benefit.
MIN_AUTO_FRAME_RATE_HZ = 50
MAX_AUTO_FRAME_RATE_HZ = 60


@dataclass
class LedStripTraits:
    channels: str  # "RGB", "RGBW", "WRGB", etc.
    output_op_type: cg.MockObjClass  # a subclass of OutputOp
    num_leds: int  # number of individually addressable pixels in the LED strip
    max_frame_rate: float  # maximum frame rate considering the number of pixels and device capabilities

    @property
    def has_w_channel(self):
        return "W" in self.channels

    @property
    def num_channels(self):
        return len(self.channels)


@dataclass
class LutParams:
    gamma: float = 1.0  # must be greater than 0.0
    min_level: int = 1  # must be between 1 and 255
    max_level: int = 255  # must be between min_level and 255

    def key(self) -> tuple[float, int, int]:
        return (self.gamma, self.min_level, self.max_level)


@dataclass
class LedStripData:
    traits: dict[ID, LedStripTraits] = field(default_factory=dict)
    luts: dict[tuple[float, int, int], cg.Expression] = field(default_factory=dict)


def _get_data() -> LedStripData:
    if DOMAIN not in CORE.data:
        CORE.data[DOMAIN] = LedStripData()
    return CORE.data[DOMAIN]


def get_led_strip_traits(led_strip_id: ID) -> LedStripTraits:
    return _get_data().traits[led_strip_id]


def register_led_strip_traits(led_strip_id: ID, traits: LedStripTraits):
    data = _get_data()
    data.traits[led_strip_id] = traits


LED_STRIP_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LedStrip),
    }
)


async def register_led_strip(var: cg.MockObj, config: ConfigType) -> None:
    """Register an LedStrip instance with the platform."""


def _validate_input_options(input_config: ConfigType) -> ConfigType:
    return input_config


def _validate_lut_options(lut_config: ConfigType) -> ConfigType:
    lut_gamma = lut_config[CONF_GAMMA_CORRECT]
    lut_min_level = lut_config[CONF_MIN_LEVEL]
    lut_max_level = lut_config[CONF_MAX_LEVEL]
    lut_method = lut_config.get(CONF_METHOD)
    if lut_max_level < lut_min_level:
        raise cv.Invalid("'max_level' must be greater than or equal to 'min_level'")
    if lut_gamma == 1.0 and lut_min_level == 1 and lut_max_level == 255:
        # Helpfully switch to a more optimal method when available.
        if lut_method is not None and lut_method.enum_value.base != LutOpNone.base:
            _LOGGER.warning(
                "Using LUT method 'none' instead of '%s' because it is optimal for the provided 'lut' options",
                lut_method,
            )
        lut_config[CONF_METHOD] = cv.enum(LUT_METHODS)("none")
    elif CONF_METHOD not in lut_config:
        lut_config[CONF_METHOD] = cv.enum(LUT_METHODS)("fast")
    return lut_config


def _validate_dither_options(dither_config: ConfigType) -> ConfigType:
    dither_bits = dither_config[CONF_BITS]
    dither_method = dither_config.get(CONF_METHOD)
    if dither_bits == 0:
        # Helpfully switch to a more optimal method when available.
        if (
            dither_config is not None
            and dither_method.enum_value.base != DitherOpNone.base
        ):
            _LOGGER.warning(
                "Using dither method 'none' instead of '%s' because it is optimal for the provided 'dither' options",
                dither_method,
            )
        dither_config[CONF_METHOD] = cv.enum(DITHER_METHODS)("none")
    elif CONF_METHOD not in dither_config:
        dither_config[CONF_METHOD] = cv.enum(DITHER_METHODS)("temporal")
    return dither_config


def _get_renderer_type(renderer_config: ConfigType) -> cg.MockObjClass:
    if renderer_config[CONF_INPUT][CONF_METHOD].enum_value.base == InputOpLerp.base:
        return LerpContinuousRenderer
    if (
        renderer_config[CONF_DITHER][CONF_METHOD].enum_value.base
        == DitherOpTemporal.base
    ):
        return FastContinuousRenderer
    return ImmediateRenderer


def _validate_renderer_options(renderer_config: ConfigType) -> ConfigType:
    if (
        not _get_renderer_type(renderer_config).inherits_from(ContinuousRenderer)
        and renderer_config[CONF_FRAME_RATE] != "auto"
    ):
        _LOGGER.warning(
            "'frame_rate' will be ignored with the provided 'renderer' options; omit 'frame_rate' or use 'auto'"
        )
        renderer_config[CONF_FRAME_RATE] = "auto"
    return renderer_config


LED_STRIP_CLIENT_SCHEMA = cv.Schema(
    {
        cv.GenerateID(CONF_LED_STRIP_ID): cv.use_id(LedStrip),
        cv.GenerateID(CONF_LUT_ID): cv.declare_id(Lut),
        cv.GenerateID(CONF_PIPELINE_ID): cv.declare_id(Pipeline),
        cv.GenerateID(CONF_RENDERER_ID): cv.declare_id(Renderer),
        cv.Optional(CONF_RENDERER, default={}): cv.All(
            cv.Schema(
                {
                    cv.Optional(CONF_INPUT, default={}): cv.All(
                        cv.Schema(
                            {
                                cv.Optional(CONF_METHOD, default="fast"): cv.enum(
                                    INPUT_METHODS, lower=True
                                ),
                            }
                        ),
                        _validate_input_options,
                    ),
                    cv.Optional(CONF_LUT, default={}): cv.All(
                        cv.Schema(
                            {
                                cv.Optional(CONF_METHOD): cv.enum(
                                    LUT_METHODS, lower=True
                                ),
                                cv.Optional(
                                    CONF_GAMMA_CORRECT, default=1.0
                                ): cv.positive_float,
                                cv.Optional(CONF_MIN_LEVEL, default=1): cv.int_range(
                                    min=1, max=255
                                ),
                                cv.Optional(CONF_MAX_LEVEL, default=255): cv.int_range(
                                    min=1, max=255
                                ),
                            }
                        ),
                        _validate_lut_options,
                    ),
                    cv.Optional(CONF_DITHER, default={}): cv.All(
                        cv.Schema(
                            {
                                cv.Optional(CONF_METHOD): cv.enum(
                                    DITHER_METHODS, lower=True
                                ),
                                cv.Optional(CONF_BITS, default=0): cv.int_range(
                                    min=0, max=4
                                ),
                            }
                        ),
                        _validate_dither_options,
                    ),
                    cv.Optional(CONF_FRAME_RATE, default="auto"): cv.Any(
                        "auto", cv.All(cv.framerate, cv.int_range(1, 1000))
                    ),
                    cv.Optional(CONF_FRAME_STATS, default="never"): cv.Any(
                        "never", cv.positive_time_period_seconds
                    ),
                }
            ),
            _validate_renderer_options,
        ),
    }
)


def _generate_lut(params: LutParams) -> list[HexInt]:
    """Generate a 257-entry uq8_8_t color correction lookup table.

    This look-up table transforms scaled linear RGB(W) intensities in the range of 0 to 255 to the non-linear
    domain of an LED strip by applying a non-decreasing monotonic function such as exponential gamma or some
    other curve as appropriate for the hardware.

    The 0th entry must be 0 and subsequent entries should be non-zero.
    The 255th entry must be no larger than 1.0 (0xff00 in binary representation).
    The 256th entry must equal the 255th entry to allow indexing one entry beyond the maximum value.
    """
    if (
        params.gamma <= 0.0
        or params.min_level < 1
        or params.max_level > 255
        or params.max_level < params.min_level
    ):
        raise ValueError(f"Invalid LUT parameters: {params}")

    table = [0]
    table += [
        ((i / 255.0) ** params.gamma) * (params.max_level - params.min_level)
        + params.min_level
        for i in range(1, 256)
    ]
    table += table[-1:]
    return [HexInt(int(round(x * 0x100))) for x in table]


def _lut_initializer(params: LutParams) -> str:
    """C++ initializer for a led_strip::renderer::Lut."""
    lut = ", ".join(f"0x{int(v):04X}" for v in _generate_lut(params))
    return f"{{{lut}}}"


def _get_or_create_lut(params: LutParams, lut_id: ID) -> cg.RawExpression:
    data = _get_data()
    params_key = params.key()
    if (lut := data.luts.get(params_key)) is not None:
        return lut

    cg.add(
        cg.RawStatement(
            f"static constexpr led_strip::renderer::Lut {lut_id} PROGMEM = "
            f"{_lut_initializer(params)};"
        )
    )
    lut = cg.RawExpression(f"&{lut_id}")
    data.luts[params_key] = lut
    return lut


async def new_led_strip_renderer(client_config: ConfigType) -> cg.MockObj:
    led_strip_id = client_config[CONF_LED_STRIP_ID]
    led_strip_var = await cg.get_variable(led_strip_id)
    led_strip_traits = get_led_strip_traits(led_strip_id)

    renderer_config = client_config[CONF_RENDERER]
    renderer_type = _get_renderer_type(renderer_config)

    input_config = renderer_config[CONF_INPUT]
    input_op_type = input_config[CONF_METHOD].enum_value

    lut_config = renderer_config[CONF_LUT]
    lut_op_type = lut_config[CONF_METHOD].enum_value
    if lut_op_type.base == LutOpNone.base:
        lut_op_expr = cg.StructInitializer(lut_op_type)
    else:
        lut_params = LutParams()
        lut_params.gamma = lut_config[CONF_GAMMA_CORRECT]
        lut_params.min_level = lut_config[CONF_MIN_LEVEL]
        lut_params.max_level = lut_config[CONF_MAX_LEVEL]
        lut = _get_or_create_lut(lut_params, client_config[CONF_LUT_ID])
        lut_op_expr = cg.StructInitializer(lut_op_type, ("lut", lut))

    dither_config = renderer_config[CONF_DITHER]
    dither_op_type = dither_config[CONF_METHOD].enum_value
    dither_temporal_scale = 1
    if dither_op_type.base == DitherOpNone.base:
        dither_op_expr = cg.StructInitializer(dither_op_type)
    else:
        dither_bits = dither_config[CONF_BITS]
        dither_op_expr = cg.StructInitializer(
            dither_op_type, ("dither_bits", dither_bits)
        )
        if dither_op_type.base == DitherOpTemporal.base:
            dither_temporal_scale = 2**dither_bits

    channel_op_type = ChannelOp.template(lut_op_type, dither_op_type)
    channel_op_expr = cg.StructInitializer(
        channel_op_type, ("lut_op", lut_op_expr), ("dither_op", dither_op_expr)
    )

    render_op_type = (
        RenderOpRGBW if led_strip_traits.has_w_channel else RenderOpRGB
    ).template(channel_op_type)
    render_op_expr = cg.StructInitializer(
        render_op_type, ("channel_op", channel_op_expr)
    )

    output_op_type = led_strip_traits.output_op_type

    pipeline_var = cg.new_Pvariable(
        client_config[CONF_PIPELINE_ID],
        cg.TemplateArguments(input_op_type, render_op_type, output_op_type),
        render_op_expr,
    )

    if renderer_type.inherits_from(ContinuousRenderer):
        cg.add_define("USE_LED_STRIP_CONTINUOUS_RENDERER")
        esp32.add_idf_sdkconfig_option(
            "CONFIG_ESP_TIMER_SUPPORTS_ISR_DISPATCH_METHOD", True
        )

        frame_rate = renderer_config[CONF_FRAME_RATE]
        max_frame_rate = led_strip_traits.max_frame_rate
        if frame_rate == "auto":
            # Automatically choose a frame rate to prevent the appearance of flickering.
            frame_rate_scale = dither_temporal_scale
            min_auto_frame_rate = MIN_AUTO_FRAME_RATE_HZ * frame_rate_scale
            max_auto_frame_rate = MAX_AUTO_FRAME_RATE_HZ * frame_rate_scale
            if max_auto_frame_rate <= max_frame_rate:
                frame_rate = max_auto_frame_rate
            elif min_auto_frame_rate <= max_frame_rate:
                frame_rate = max_frame_rate
            else:
                # Choose the minimum auto frame rate even if it is larger than the maximum frame rate that the
                # LED strip component reported.  This will let the user experience the issue and log a warning
                # to help them fix it.
                frame_rate = min_auto_frame_rate
        if frame_rate > max_frame_rate:
            _LOGGER.warning(
                "LED strip '%s' can only support a maximum frame rate of %.1f Hz but %.1f Hz was requested; flickering may occur.\n"
                "Note: The maximum frame rate may depend on the LED strip's type, length, and buffer size.",
                led_strip_id,
                max_frame_rate,
                frame_rate,
            )
        renderer_args = [frame_rate]
    else:
        renderer_args = []

    renderer_var = cg.Pvariable(
        client_config[CONF_RENDERER_ID],
        renderer_type.new(led_strip_var, *renderer_args, pipeline_var),
        renderer_type,
    )

    if (frame_stats := renderer_config[CONF_FRAME_STATS]) != "never":
        cg.add_define("USE_LED_STRIP_FRAME_STATS")
        cg.add(
            renderer_var.set_frame_stats_report_interval_us(
                frame_stats.total_microseconds
            )
        )

    return renderer_var
