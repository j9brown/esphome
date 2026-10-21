#pragma once

#ifdef USE_ESP32
#include <soc/soc_caps.h>
#endif

#if defined(USE_ESP32) && SOC_I2S_SUPPORTS_TDM

#include <atomic>
#include <cstdint>
#include <tuple>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/led_strip/led_strip.h"
#include "esphome/components/light/channel_colors.h"

#include <driver/i2s_types.h>

namespace esphome::i2s_clockless_led_strip {
namespace renderer {

struct I2SOutputOp : public led_strip::renderer::OutputOp {
  inline void write_next_channel(uint8_t value) {
    *(i2s_data++) = 0b10010010 | ((value & 0x80) >> 1) | ((value & 0x40) >> 3) | ((value & 0x20) >> 5);
    *(i2s_data++) = 0b01001001 | ((value & 0x10) << 1) | ((value & 0x08) >> 1);
    *(i2s_data++) = 0b00100100 | ((value & 0x04) << 5) | ((value & 0x02) << 3) | ((value & 0x01) << 1);
  }

  uint8_t *i2s_data;
};

enum class Channel {
  R = 0,
  G = 1,
  B = 2,
  W = 3,
};

template<Channel c0, Channel c1, Channel c2> struct I2SOutputOpRGB : public I2SOutputOp {
  inline void write_next_pixel(uint8_t r, uint8_t g, uint8_t b) {
    auto rgb = std::make_tuple(r, g, b);
    this->write_next_channel(std::get<unsigned(c0)>(rgb));
    this->write_next_channel(std::get<unsigned(c1)>(rgb));
    this->write_next_channel(std::get<unsigned(c2)>(rgb));
  }
};

template<Channel c0, Channel c1, Channel c2, Channel c3> struct I2SOutputOpRGBW : public I2SOutputOp {
  inline void write_next_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    auto rgbw = std::make_tuple(r, g, b, w);
    this->write_next_channel(std::get<unsigned(c0)>(rgbw));
    this->write_next_channel(std::get<unsigned(c1)>(rgbw));
    this->write_next_channel(std::get<unsigned(c2)>(rgbw));
    this->write_next_channel(std::get<unsigned(c3)>(rgbw));
  }
};

}  // namespace renderer

class I2SClocklessLedStrip2 final : public esphome::Component, public led_strip::LedStrip {
 public:
  I2SClocklessLedStrip2(uint8_t pin, uint16_t num_leds, light::ChannelColors channel_colors);

  void dump_config() override;

  float get_setup_priority() const override;
  void setup() override;

  led_strip::LedStripTraits get_traits() const override;
  led_strip::LedStripStatus begin_frame(void *output_op) override;
  void end_frame(led_strip::renderer::OutputOp *output_op) override;

 protected:
  static bool i2s_on_sent_callback(i2s_chan_handle_t handle, i2s_event_data_t *event, void *user_ctx);

  const size_t i2s_data_bytes_;
  uint8_t *i2s_data_{nullptr};
  size_t i2s_data_sent_{0};
  i2s_chan_handle_t tx_handle_{};
  const light::ChannelColors channel_colors_;
  const uint16_t num_leds_;
  const uint8_t pin_;
  std::atomic<bool> i2s_data_ready_{false};
};

}  // namespace esphome::i2s_clockless_led_strip

#endif
