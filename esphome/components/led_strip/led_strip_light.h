#pragma once

#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/light/addressable_light.h"
#include "led_strip_renderer.h"

namespace esphome::led_strip {

class LedStripLight final : public light::AddressableLight {
 public:
  explicit LedStripLight(renderer::Renderer *renderer);

  void dump_config() override;
  float get_setup_priority() const override;
  void setup() override;

  light::LightTraits get_traits() override;

  int32_t size() const override { return this->num_pixels(); }
  void clear_effect_data() override;

  std::unique_ptr<light::LightTransformer> create_default_transition() override;
  void update_state(light::LightState *state) override;
  void write_state(light::LightState *state) override;

 protected:
  inline bool has_w_channel() const { return this->renderer_->traits().has_w_channel; }
  inline size_t num_pixels() const { return this->renderer_->traits().num_pixels; }
  inline size_t color_data_bytes() const { return this->renderer_->color_data_bytes(); }
  inline size_t effect_data_bytes() const { return this->num_pixels(); }

  bool allocate_buffers_();

  light::ESPColorView get_view_internal(int32_t index) const override;

  renderer::Renderer *const renderer_;

  uint8_t *effect_data_{nullptr};
  uint8_t *color_data_{nullptr};
};

}  // namespace esphome::led_strip
