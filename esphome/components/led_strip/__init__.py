from dataclasses import dataclass, field

import esphome.codegen as cg
import esphome.config_validation as cv
from esphome.core import CORE, ID, HexInt
from esphome.types import ConfigType

CODEOWNERS = ["@j9brown"]
IS_PLATFORM_COMPONENT = True
DOMAIN = "led_strip"


led_strip_ns = cg.esphome_ns.namespace("led_strip")
LedStrip = led_strip_ns.class_("LedStrip")


LED_STRIP_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(LedStrip),
    }
)


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
    min_output: int = 1  # must be between 0 and 255
    max_output: int = 255  # must be between min_output and 255

    def key(self) -> tuple[float, int, int]:
        return (self.gamma, self.min_output, self.max_output)


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
        or params.min_output < 0
        or params.max_output > 255
        or params.max_output < params.min_output
    ):
        raise ValueError(f"Invalid LUT parameters: {params}")

    table = [0]
    table += [
        max(((i / 255.0) ** params.gamma) * params.max_output, params.min_output)
        for i in range(1, 256)
    ]
    table += table[-1:]
    return [HexInt(int(round(x * 0x100))) for x in table]


def _lut_initializer(params: LutParams) -> str:
    """C++ initializer for a led_strip::renderer::Lut."""
    lut = ", ".join(f"0x{int(v):04X}" for v in _generate_lut(params))
    return f"{{{lut}}}"


def get_or_create_lut(params: LutParams, lut_id: ID) -> cg.RawExpression:
    data = _get_data()
    params_key = params.key()
    if (lut := data.luts.get(params_key)) is not None:
        return lut

    # lut_id is generated and resolved against every declared ID, so it can't collide with a
    # YAML ID; lights sharing the same parameters reuse the first light's table.
    cg.add(
        cg.RawStatement(
            f"static constexpr led_strip::renderer::Lut {lut_id} PROGMEM = "
            f"{_lut_initializer(params)};"
        )
    )
    lut = cg.RawExpression(f"&{lut_id}")
    data.luts[params_key] = lut
    return lut


async def register_led_strip(var: cg.MockObj, config: ConfigType):
    pass
