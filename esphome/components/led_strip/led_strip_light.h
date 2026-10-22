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
      : renderer_(renderer), color_scale_(color_scale) {}

  void dump_config() override;
  float get_setup_priority() const override;
  void setup() override;

  light::LightTraits get_traits() override;

  int32_t size() const override { return this->num_leds(); }
  void clear_effect_data() override;

  std::unique_ptr<light::LightTransformer> create_default_transition() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

 protected:
  inline bool has_w_channel() const { return this->renderer_->traits().has_w_channel; }
  inline size_t num_leds() const { return this->renderer_->traits().num_leds; }
  inline size_t color_data_bytes() const { return this->renderer_->color_data_bytes(); }
  inline size_t effect_data_bytes() const { return this->num_leds(); }

  bool allocate_buffers_();

  light::ESPColorView get_view_internal(int32_t index) const override;

  renderer::Renderer *const renderer_;
  const ColorScaleFloat color_scale_;

  uint8_t *effect_data_{nullptr};
  uint8_t *color_data_{nullptr};
};

}  // namespace esphome::led_strip
