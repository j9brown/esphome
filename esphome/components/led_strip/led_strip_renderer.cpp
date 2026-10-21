#include "led_strip_renderer.h"

#include "esphome/core/log.h"

namespace esphome::led_strip::renderer {

ESPHOME_LOG_TAG(TAG, "led_strip");

constexpr size_t calc_color_data_bytes(const LedStripTraits &traits) {
#ifdef USE_LED_STRIP_W_CHANNEL
  if (traits.has_w_channel)
    return traits.num_pixels * 4;
#endif
  return traits.num_pixels * 3;
}

Renderer::Renderer(LedStrip *led_strip)
    : led_strip_(led_strip),
      led_strip_traits_(led_strip->get_traits()),
      color_data_bytes_(calc_color_data_bytes(this->led_strip_traits_)) {}

void Renderer::dump_config() {}

bool Renderer::setup() { return true; }

LedStripStatus ImmediateRenderer::show(const uint8_t *color_data, uq24_8_t scale) {
  LedStripStatus status = this->led_strip_->begin_frame();
  if (status == LedStripStatus::READY) {
    this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_pixels, InputOpFast{.color_data = color_data},
                            scale);
    this->led_strip_->present_frame();
  }
  return status;
}

}  // namespace esphome::led_strip::renderer
