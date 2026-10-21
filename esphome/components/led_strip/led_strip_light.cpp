#include "led_strip_light.h"

#include <algorithm>

#include "esphome/core/log.h"

namespace esphome::led_strip {

ESPHOME_LOG_TAG(TAG, "led_strip");

constexpr const char *const ERROR_ALLOCATION = "Allocation error";
constexpr const char *const ERROR_RENDERER = "Renderer error";

LedStripLight::LedStripLight(renderer::Renderer *renderer) : renderer_(renderer) {}

void LedStripLight::dump_config() {
  ESP_LOGCONFIG(TAG, "LED Strip Light:\n");
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
#ifdef USE_LED_STRIP_W_CHANNEL
  if (this->has_w_channel()) {
    traits.set_supported_color_modes({light::ColorMode::RGB_WHITE, light::ColorMode::WHITE});
    return traits;
  }
#endif
  traits.set_supported_color_modes({light::ColorMode::RGB});
  return traits;
}

void LedStripLight::write_state(light::LightState *state) {
  if (this->is_failed())
    return;

  const auto scale = std::clamp<renderer::uq24_8_t>(
      state->current_values.get_brightness() * state->current_values.get_state() * 0x100, 0, 0x100);
  LedStripStatus status = this->renderer_->show(this->color_data_, scale);
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
#ifdef USE_LED_STRIP_W_CHANNEL
  if (this->has_w_channel()) {
    uint8_t *led = this->color_data_ + index * 4;
    return {led, led + 1, led + 2, led + 3, &this->effect_data_[index], &this->identity_correction_};
  }
#endif
  uint8_t *led = this->color_data_ + index * 3;
  return {led, led + 1, led + 2, nullptr, &this->effect_data_[index], &this->identity_correction_};
}

}  // namespace esphome::led_strip
