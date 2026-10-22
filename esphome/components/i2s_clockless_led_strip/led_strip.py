from dataclasses import dataclass
import math

from esphome import pins
import esphome.codegen as cg
from esphome.components import esp32, led_strip, light
from esphome.components.const import CONF_CHANNEL_COLORS
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NUM_LEDS, CONF_PIN
from esphome.core import CORE
from esphome.types import ConfigType

from . import DOMAIN

CODEOWNERS = ["@j9brown"]
DEPENDENCIES = ["esp32"]

i2s_clockless_led_strip_ns = cg.esphome_ns.namespace("i2s_clockless_led_strip")
I2SClocklessLedStrip2 = i2s_clockless_led_strip_ns.class_(
    "I2SClocklessLedStrip2",
    led_strip.LedStrip,
    cg.Component,
)

i2s_clockless_led_strip_renderer_ns = i2s_clockless_led_strip_ns.namespace("renderer")
Channel = i2s_clockless_led_strip_renderer_ns.enum("Channel", is_class=True)
I2SOutputOpRGB = i2s_clockless_led_strip_renderer_ns.struct("I2SOutputOpRGB")
I2SOutputOpRGBW = i2s_clockless_led_strip_renderer_ns.struct("I2SOutputOpRGBW")


CONF_MAX_BUFFER_DURATION = "max_buffer_duration"

# These variants define SOC_I2S_SUPPORTS_TDM
I2S_TDM_PORTS = {
    esp32.VARIANT_ESP32C3: 1,
    esp32.VARIANT_ESP32C5: 1,
    esp32.VARIANT_ESP32C6: 1,
    esp32.VARIANT_ESP32C61: 1,
    esp32.VARIANT_ESP32H2: 1,
    esp32.VARIANT_ESP32H4: 1,
    esp32.VARIANT_ESP32P4: 3,
    esp32.VARIANT_ESP32S3: 2,
}

# LED strip data rate in bits per seconds.
LED_STRIP_BIT_RATE_HZ = 800000

# Duration of the LED strip reset signal in microseconds.
# Minimum 50 us for WS2811, 80 us for SK6812, 280 us for WS2812B-V5.
LED_STRIP_RESET_DURATION_US = 300

# Each bit of LED strip data gets expanded into 3 bits of I2S data, where a 0-bit expands into 100
# (1/3 duty cycle) and a 1-bit expands into 110 (2/3 duty cycle.  Each byte of LED strip data becomes
# one 24-bit I2S sample to be transmitted at a rate of 100000 samples per second in I2S TDM mode.
I2S_SAMPLE_RATE_HZ = int(LED_STRIP_BIT_RATE_HZ / 8)
I2S_BYTES_PER_SAMPLE = 3


# Determine how many I2S samples are needed to feed the output for the specified duration and vice-versa.
def _num_i2s_samples_from_duration_us(duration_us: float) -> int:
    return int(math.ceil(duration_us * I2S_SAMPLE_RATE_HZ / 1000000))


def _num_i2s_samples_to_duration_us(samples: int) -> float:
    return samples * 1000000 / I2S_SAMPLE_RATE_HZ


# Determine the number of I2S samples of encoded LED data to send.
def _num_i2s_data_samples(num_leds: int, num_channels: int) -> int:
    return num_leds * num_channels


# Determine the number of zero I2S samples to pad after the LED data to encode the LED strip reset signal.
def _num_i2s_reset_samples() -> int:
    return _num_i2s_samples_from_duration_us(LED_STRIP_RESET_DURATION_US)


# Balance between larger buffers and lower latency.
MIN_I2S_BUFFER_DURATION_US = 1000
MAX_I2S_BUFFER_DURATION_US = 2500


# Determine the number of I2S samples per DMA buffer to ensure that the interrupt-driven I2S on_sent callback
# doesn't run too frequently.  For example, if the buffers were sized to hold just 1 RGBW pixel then they would be
# recycled approximately every 40 us (25000 Hz) which is way too fast.  The buffer duration also imposes an upper
# bound on the maximum LED strip refresh rate.  Round down to a multiple of 3 for `dma_frame_num` as required by
# the ESP-IDF programming guide when using 24-bit samples.
def _num_i2s_buffer_samples(max_buffer_duration_us: float) -> int:
    nominal_buffer_duration_us = min(
        max(max_buffer_duration_us, MIN_I2S_BUFFER_DURATION_US),
        MAX_I2S_BUFFER_DURATION_US,
    )
    return int(
        math.floor(_num_i2s_samples_from_duration_us(nominal_buffer_duration_us) / 3)
        * 3
    )


# An arbitrary margin to avoid dropping frames at maximum frame rate due to DMA synchronization
# and interrupt latency.
FRAME_DURATION_MARGIN_US = 200


# Estimate the minimum amount of time it will take to send a frame taking into account the size of the buffer.
def _min_frame_duration_us(
    num_leds: int, num_channels: int, buffer_samples: int
) -> float:
    data_samples = _num_i2s_data_samples(num_leds, num_channels)
    reset_samples = _num_i2s_reset_samples()
    num_samples = data_samples + reset_samples
    actual_samples = int(math.ceil(num_samples / buffer_samples) * buffer_samples)
    return _num_i2s_samples_to_duration_us(actual_samples) + FRAME_DURATION_MARGIN_US


def _register_led_strip_traits(config: ConfigType) -> ConfigType:
    channels = config[CONF_CHANNEL_COLORS]

    output_op_type = I2SOutputOpRGBW if "W" in channels else I2SOutputOpRGB
    output_op_type = output_op_type.template(*[getattr(Channel, x) for x in channels])

    num_leds = config[CONF_NUM_LEDS]
    num_channels = len(channels)
    i2s_buffer_samples = _num_i2s_buffer_samples(
        config[CONF_MAX_BUFFER_DURATION].total_microseconds
    )
    max_frame_rate = 1000000 / _min_frame_duration_us(
        num_leds, num_channels, i2s_buffer_samples
    )

    traits = led_strip.LedStripTraits(
        channels=channels,
        output_op_type=output_op_type,
        num_leds=num_leds,
        max_frame_rate=max_frame_rate,
    )
    led_strip.register_led_strip_traits(config[CONF_ID], traits)
    return config


CONFIG_SCHEMA = cv.All(
    esp32.only_on_variant(
        supported=list(I2S_TDM_PORTS.keys()),
        msg_prefix="I2S Clockless LED Strip",
    ),
    cv.Schema(
        {
            cv.GenerateID(CONF_ID): cv.declare_id(I2SClocklessLedStrip2),
            cv.Required(CONF_PIN): pins.internal_gpio_output_pin_number,
            cv.Required(CONF_NUM_LEDS): cv.positive_not_null_int,
            cv.Required(CONF_CHANNEL_COLORS): light.validate_channel_colors,
            cv.Optional(
                CONF_MAX_BUFFER_DURATION, default=f"{MAX_I2S_BUFFER_DURATION_US}us"
            ): cv.positive_time_period_microseconds,
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _register_led_strip_traits,
)


@dataclass
class I2SClocklessLedStrip2Data:
    num_ports: int = 0


def _get_data() -> I2SClocklessLedStrip2Data:
    if DOMAIN not in CORE.data:
        CORE.data[DOMAIN] = I2SClocklessLedStrip2Data()
    return CORE.data[DOMAIN]


def _final_validate(_: ConfigType) -> None:
    _get_data().num_ports += 1
    variant = esp32.get_esp32_variant()
    num_ports_available = I2S_TDM_PORTS.get(variant, 0)
    if _get_data().num_ports > num_ports_available:
        raise cv.Invalid(
            f"Only {num_ports_available} I2S TDM ports are supported on {variant}"
        )


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config: ConfigType) -> None:
    esp32.include_builtin_idf_component("esp_driver_i2s")
    esp32.add_idf_sdkconfig_option("CONFIG_I2S_ISR_IRAM_SAFE", True)

    num_leds = config[CONF_NUM_LEDS]
    num_channels = len(config[CONF_CHANNEL_COLORS])
    i2s_data_bytes = (
        _num_i2s_data_samples(num_leds, num_channels) * I2S_BYTES_PER_SAMPLE
    )
    i2s_reset_bytes = _num_i2s_reset_samples() * I2S_BYTES_PER_SAMPLE
    i2s_buffer_samples = _num_i2s_buffer_samples(
        config[CONF_MAX_BUFFER_DURATION].total_microseconds
    )
    i2s_buffer_duration_us = _num_i2s_samples_to_duration_us(i2s_buffer_samples)

    var = cg.new_Pvariable(
        config[CONF_ID],
        config[CONF_PIN],
        config[CONF_NUM_LEDS],
        light.channel_colors_struct(config[CONF_CHANNEL_COLORS]),
        I2S_SAMPLE_RATE_HZ,
        i2s_data_bytes,
        i2s_reset_bytes,
        i2s_buffer_samples,
        i2s_buffer_duration_us,
    )

    await led_strip.register_led_strip(var, config)
    await cg.register_component(var, config)
