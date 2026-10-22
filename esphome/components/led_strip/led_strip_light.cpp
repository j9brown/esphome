#include "led_strip_light.h"

#include <algorithm>

#include "esphome/components/light/transformers.h"
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
}

bool LedStripLight::allocate_buffers_() {
  RAMAllocator<uint8_t> allocator;
  this->color_data_ = allocator.allocate(this->color_data_bytes());
  if (this->color_data_ != nullptr) {
    this->effect_data_ = allocator.allocate(this->effect_data_bytes());
    if (this->effect_data_ != nullptr) {
      memset(this->color_data_, 0, this->color_data_bytes());
      memset(this->effect_data_, 0, this->effect_data_bytes());
      return true;
    }
    allocator.deallocate(this->color_data_, this->color_data_bytes());
    this->color_data_ = nullptr;
  }
  return false;
}

light::LightTraits LedStripLight::get_traits() {
  auto traits = light::LightTraits();
  if (this->has_w_channel()) {
    traits.set_supported_color_modes({light::ColorMode::RGB_WHITE, light::ColorMode::WHITE});
    return traits;
  }
  traits.set_supported_color_modes({light::ColorMode::RGB});
  return traits;
}

std::unique_ptr<light::LightTransformer> LedStripLight::create_default_transition() {
  // Override the AddressableLight's implementation to only animate the light values and apply them
  // in update_state.  The AddressableLightTransformer pre-scales the color values in the frame
  // buffer which doesn't work.
  return make_unique<light::LightTransitionTransformer>();
}

void LedStripLight::update_state(light::LightState *state) {
  // Override the AddressableLight's implementation to preserve full-scale values in the frame buffer
  // because scaling will be applied by the renderer instead.  Also keeps the base class's color
  // correction at its default (identity transformation) value.
  if (!this->is_effect_active()) {
    const auto &val = state->current_values;
    this->all() = Color(light::to_uint8_scale(val.get_red()), light::to_uint8_scale(val.get_green()),
                        light::to_uint8_scale(val.get_blue()), light::to_uint8_scale(val.get_white()));
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

  LedStripStatus status = this->renderer_->show(this->color_data_, color_scale, done);
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

void LedStripLight::clear_effect_data() { memset(this->effect_data_, 0, this->effect_data_bytes()); }

light::ESPColorView LedStripLight::get_view_internal(int32_t index) const {
  if (this->has_w_channel()) {
    uint8_t *led = this->color_data_ + index * 4;
    return {led, led + 1, led + 2, led + 3, &this->effect_data_[index], &this->correction_};
  }
  uint8_t *led = this->color_data_ + index * 3;
  return {led, led + 1, led + 2, nullptr, &this->effect_data_[index], &this->correction_};
}

}  // namespace esphome::led_strip
