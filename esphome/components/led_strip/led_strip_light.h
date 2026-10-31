#pragma once

#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/light/addressable_light.h"
#include "led_strip_renderer.h"

namespace esphome::led_strip {

/// Scales the colors that are read from the input color data.
/// Each factor must be between 0.0 and 1.0.
struct ColorScaleFloat {
  float r{1.f};
  float g{1.f};
  float b{1.f};
  float w{1.f};
};

class LedStripLight final : public light::AddressableLight {
 public:
  LedStripLight(renderer::Renderer *renderer, ColorScaleFloat color_scale)
      : renderer_(renderer),
        color_scale_(color_scale),
        internal_accessor_(nullptr, nullptr,
                           {0, 1, 2, this->has_w_channel_() ? uint8_t(3) : light::ChannelColors::NO_WHITE}) {}

  void dump_config() override;
  float get_setup_priority() const override;
  void setup() override;

  light::LightTraits get_traits() override;

  int32_t size() const override { return this->num_leds_(); }
  void clear_effect_data() override;

  std::unique_ptr<light::LightTransformer> create_default_transition() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

 protected:
  inline bool has_w_channel_() const { return this->renderer_->led_strip_traits().has_w_channel; }
  inline size_t num_leds_() const { return this->renderer_->led_strip_traits().num_leds; }
  inline size_t color_data_bytes_() const { return this->renderer_->color_data_bytes(); }

  renderer::Renderer *const renderer_;
  const ColorScaleFloat color_scale_;

  light::RawBufferColorAccessor internal_accessor_;
  bool active_{};
};

class LedStripLightTransformer : public light::LightTransformer {
 public:
  LedStripLightTransformer(LedStripLight &light) : light_(light) {}

  void start() override;
  std::optional<light::LightColorValues> apply() override;

 protected:
  LedStripLight &light_;
  float last_transition_progress_{0.0f};
  std::optional<Color> target_color_{};
};

}  // namespace esphome::led_strip
