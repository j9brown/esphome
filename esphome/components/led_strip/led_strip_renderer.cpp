#include "led_strip_renderer.h"

#include "esphome/core/log.h"

namespace esphome::led_strip::renderer {

ESPHOME_LOG_TAG(TAG, "led_strip");

constexpr size_t calc_color_data_bytes(const LedStripTraits &traits) {
  return traits.num_pixels * (traits.has_w_channel ? 4 : 3);
}

Renderer::Renderer(LedStrip *led_strip)
    : led_strip_(led_strip),
      led_strip_traits_(led_strip->get_traits()),
      color_data_bytes_(calc_color_data_bytes(this->led_strip_traits_)) {}

void Renderer::dump_config() {}

bool Renderer::setup() { return true; }

LedStripStatus ImmediateRenderer::show(const uint8_t *color_data, uq24_8_t scale) {
  return this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_pixels,
                                 InputOpFast{.color_data = color_data}, scale);
}

}  // namespace esphome::led_strip::renderer
