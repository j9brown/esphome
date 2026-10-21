#pragma once

#ifdef USE_ESP32
#include <soc/soc_caps.h>
#endif

#if defined(USE_ESP32) && SOC_I2S_SUPPORTS_TDM

#include <atomic>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/led_strip/led_strip.h"
#include "esphome/components/light/channel_colors.h"

#include <driver/i2s_types.h>

namespace esphome::i2s_clockless_led_strip {

class I2SClocklessLedStrip2 final : public esphome::Component, public led_strip::LedStrip {
 public:
  I2SClocklessLedStrip2(uint8_t pin, uint16_t num_leds, light::ChannelColors channel_colors);

  void dump_config() override;

  float get_setup_priority() const override;
  void setup() override;

  led_strip::LedStripTraits get_traits() const override;
  led_strip::LedStripStatus begin_frame() override;
  void push_pixel(led_strip::LedColor color) override;
  void present_frame() override;

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
