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
    has_w_channel: bool = False


@dataclass
class LedStripData:
    traits: dict[ID, LedStripTraits] = field(default_factory=dict)
    luts: dict[float, cg.Expression] = field(default_factory=dict)


def _get_data() -> LedStripData:
    if DOMAIN not in CORE.data:
        CORE.data[DOMAIN] = LedStripData()
    return CORE.data[DOMAIN]


def get_led_strip_traits(led_strip_id: ID) -> LedStripTraits:
    return _get_data().traits[led_strip_id]


def register_led_strip_traits(led_strip_id: ID, traits: LedStripTraits):
    data = _get_data()
    data.traits[led_strip_id] = traits


def _generate_lut(gamma_correct: float) -> list[HexInt]:
    """Generate a 257-entry uq8_8_t color correction lookup table.

    This look-up table transforms scaled linear RGB(W) intensities in the range of 0 to 255 to the non-linear
    domain of an LED strip by applying a non-decreasing monotonic function such as exponential gamma or some
    other curve as appropriate for the hardware.

    The 0th entry must be 0.
    The 255th entry must be no larger than 1.0 (0xff00 in binary representation).
    The 256th entry must equal the 255th entry to allow indexing one entry beyond the maximum value.
    """
    if gamma_correct > 0:
        table = [(i / 255.0) ** gamma_correct for i in range(256)]
    else:
        table = list(range(256))
    table += table[-1:]
    return [HexInt(int(round(x * 0xFF00))) for x in table]


def _lut_initializer(gamma_correct: float) -> str:
    """C++ initializer for a led_strip::renderer::Lut."""
    lut = ", ".join(f"0x{int(v):04X}" for v in _generate_lut(gamma_correct))
    return f"{{{lut}}}"


def get_or_create_lut(gamma_correct: float, lut_id: ID) -> cg.RawExpression:
    data = _get_data()
    if (lut := data.luts.get(gamma_correct)) is not None:
        return lut

    # lut_id is generated and resolved against every declared ID, so it can't collide with a
    # YAML ID; lights sharing the same parameters reuse the first light's table.
    cg.add(
        cg.RawStatement(
            f"static constexpr led_strip::renderer::Lut {lut_id} PROGMEM = "
            f"{_lut_initializer(gamma_correct)};"
        )
    )
    lut = cg.RawExpression(f"&{lut_id}")
    data.luts[gamma_correct] = lut
    return lut


async def register_led_strip(var: cg.MockObj, config: ConfigType):
    pass
