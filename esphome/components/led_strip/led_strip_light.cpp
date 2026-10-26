#include "led_strip_light.h"

#include <algorithm>

#include "esphome/components/light/esp_color_view.h"
#include "esphome/components/light/transformers.h"
#include "esphome/core/application.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::led_strip {

ESPHOME_LOG_TAG(TAG, "led_strip");

constexpr const char *const ERROR_ALLOCATION = "Allocation error";
constexpr const char *const ERROR_RENDERER = "Renderer error";

void LedStripLight::dump_config() {
  ESP_LOGCONFIG(TAG,
                "LED Strip Light:\n"
                "  Color scale: R %.1f, G %.1f, B %.1f, W %.1f",
                this->color_scale_.r, this->color_scale_.g, this->color_scale_.b, this->color_scale_.w);
  this->renderer_->dump_config();
}

float LedStripLight::get_setup_priority() const { return setup_priority::IO; }

void LedStripLight::setup() {
  if (!this->renderer_->setup()) {
    this->mark_failed(LOG_STR(ERROR_RENDERER));
    return;
  }
  if (!this->allocate_buffers_()) {
    this->mark_failed(LOG_STR(ERROR_ALLOCATION));
    return;
  }
  this->accessor_ =
      this->internal_accessor_.init(this->color_data_, this->effect_data_(),
                                    light::ChannelColors{
                                        .r = 0,
                                        .g = 1,
                                        .b = 2,
                                        .w = this->has_w_channel_() ? uint8_t(3) : light::ChannelColors::NO_WHITE,
                                    });
}

bool LedStripLight::allocate_buffers_() {
  RAMAllocator<uint8_t> allocator;
  const size_t total_size = this->color_data_bytes_() + this->effect_data_bytes_();
  this->color_data_ = allocator.allocate(total_size);
  if (this->color_data_ != nullptr) {
    memset(this->color_data_, 0, total_size);
    return true;
  }
  return false;
}

light::LightTraits LedStripLight::get_traits() {
  auto traits = light::LightTraits();
  if (this->has_w_channel_()) {
    traits.set_supported_color_modes({light::ColorMode::RGB_WHITE, light::ColorMode::WHITE});
    return traits;
  }
  traits.set_supported_color_modes({light::ColorMode::RGB});
  return traits;
}

std::unique_ptr<light::LightTransformer> LedStripLight::create_default_transition() {
  // Override the AddressableLight's implementation to preserve full-scale values in the frame buffer.
  return make_unique<LedStripLightTransformer>(*this);
}

Color light_values_to_color(const light::LightColorValues &values) {
  return Color(light::to_uint8_scale(values.get_red()), light::to_uint8_scale(values.get_green()),
               light::to_uint8_scale(values.get_blue()), light::to_uint8_scale(values.get_white()));
}

void LedStripLight::update_state(light::LightState *state) {
  // Override the AddressableLight's implementation to preserve full-scale values in the frame buffer
  // because scaling will be applied by the renderer instead.  Also keeps the base class's color
  // correction at its default (identity transformation) value.
  if (!this->is_effect_active() && !state->is_transitioning()) {
    this->all() = light_values_to_color(state->current_values);
    this->schedule_show();
  }
}

inline renderer::uq24_8_t to_renderer_scale(float scale) {
  return std::clamp<renderer::uq24_8_t>(scale * 0x100, 0, 0x100);
}

void LedStripLight::write_state(light::LightState *state) {
  if (this->is_failed())
    return;

  const auto &val = state->current_values;
  const float w_brightness = val.get_brightness() * val.get_state();
  const float rgb_brightess = val.get_color_brightness() * w_brightness;
  const renderer::ColorScale color_scale = {
      .r = to_renderer_scale(this->color_scale_.r * rgb_brightess),
      .g = to_renderer_scale(this->color_scale_.g * rgb_brightess),
      .b = to_renderer_scale(this->color_scale_.b * rgb_brightess),
      .w = to_renderer_scale(this->color_scale_.w * w_brightness),
  };
  const bool done = !val.is_on();
  const uint32_t transition_us = App.get_loop_interval();

  renderer::RendererShow show{
      .color_data = this->color_data_, .color_scale = color_scale, .transition_us = transition_us, .done = done};
  LedStripStatus status = this->renderer_->show(show);
  switch (status) {
    case LedStripStatus::READY:
      this->mark_shown_();
      break;
    case LedStripStatus::BUSY:
      this->schedule_show();
      break;
    default:
    case LedStripStatus::ERROR:
      this->mark_failed(LOG_STR(ERROR_RENDERER));
      break;
  }
}

void LedStripLight::clear_effect_data() { memset(this->effect_data_(), 0, this->effect_data_bytes_()); }

void LedStripLightTransformer::start() {
  // Only transition the overall color if no effect is running and the strip will remain on
  const auto &val = this->target_values_;
  if (!this->light_.is_effect_active() && val.get_brightness() * val.get_state() > 0.f) {
    this->target_color_ = light_values_to_color(this->target_values_);
  }
}

inline constexpr uint8_t subtract_scaled_difference(uint8_t a, uint8_t b, int32_t scale) {
  return uint8_t(int32_t(a) - (((int32_t(a) - int32_t(b)) * scale) / 256));
}

std::optional<light::LightColorValues> LedStripLightTransformer::apply() {
  const float smoothed_progress = light::LightTransformer::smoothed_progress(this->get_progress_());
  if (this->target_color_.has_value()) {
    // Progressively interpolate the color of each pixel towards the target color.
    const Color target_color = this->target_color_.value();
    const int32_t scale =
        int32_t(256.f * std::max((1.f - smoothed_progress) / (1.f - this->last_transition_progress_), 0.f));
    for (auto led : this->light_) {
      const Color color = led.get();
      led.set(Color{subtract_scaled_difference(target_color.red, color.red, scale),
                    subtract_scaled_difference(target_color.green, color.green, scale),
                    subtract_scaled_difference(target_color.blue, color.blue, scale),
                    subtract_scaled_difference(target_color.white, color.white, scale)});
    }
    this->last_transition_progress_ = smoothed_progress;
    this->light_.schedule_show();
  }
  return light::LightColorValues::lerp(this->get_start_values(), this->get_target_values(), smoothed_progress);
}

}  // namespace esphome::led_strip
