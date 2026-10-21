#include "led_strip_renderer.h"

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::led_strip::renderer {

ESPHOME_LOG_TAG(TAG, "led_strip");

#ifdef USE_ESP32
constexpr const char TASK_NAME[] = "led_strip_renderer";
constexpr size_t TASK_STACK_SIZE = 4096;
constexpr ssize_t TASK_PRIORITY = 10;
#endif

constexpr size_t calc_color_data_bytes(const LedStripTraits &traits) {
  return traits.num_pixels * (traits.has_w_channel ? 4 : 3);
}

Renderer::Renderer(LedStrip *led_strip)
    : led_strip_(led_strip),
      led_strip_traits_(led_strip->get_traits()),
      color_data_bytes_(calc_color_data_bytes(this->led_strip_traits_)) {}

void Renderer::dump_config() {}

bool Renderer::setup() { return true; }

LedStripStatus ImmediateRenderer::show(const uint8_t *color_data, uq24_8_t scale, bool done) {
  return this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_pixels,
                                 InputOpFast{.color_data = color_data}, scale);
}

#ifdef USE_LED_STRIP_CONTINUOUS_RENDERER

bool ContinuousRenderer::setup() {
  esp_timer_create_args_t timer_create_args = {
      .callback = ContinuousRenderer::on_timer_,
      .arg = this,
      .dispatch_method = ESP_TIMER_ISR,
      .name = TASK_NAME,
      .skip_unhandled_events = true,
  };
  esp_err_t err = esp_timer_create(&timer_create_args, &this->timer_handle_);
  if (err == ESP_OK) {
    this->mutex_handle_ = xSemaphoreCreateMutex();
    if (this->mutex_handle_ != nullptr) {
      xTaskCreate(ContinuousRenderer::on_task_, TASK_NAME, TASK_STACK_SIZE, this, TASK_PRIORITY, &this->task_handle_);
      if (this->task_handle_ != nullptr) {
        return true;
      }
      vSemaphoreDelete(this->mutex_handle_);
      this->mutex_handle_ = nullptr;
    }
    esp_timer_delete(this->timer_handle_);
    this->timer_handle_ = nullptr;
  }
  return false;
}

LedStripStatus ContinuousRenderer::show(const uint8_t *color_data, uq24_8_t scale, bool done) {
  xSemaphoreTake(this->mutex_handle_, portMAX_DELAY);

  const bool render_error = this->render_error_;
  bool notify = false;
  if (!render_error) {
    this->done_ = done;
    this->copy_with_mutex_(color_data, scale);
    if (!this->timer_running_) {
      esp_timer_start_periodic(this->timer_handle_, this->frame_period_us_);
      this->timer_running_ = true;
      notify = true;
    }
  }

  xSemaphoreGive(this->mutex_handle_);
  if (notify)
    xTaskNotify(this->task_handle_, 0, eNoAction);
  return render_error ? LedStripStatus::ERROR : LedStripStatus::READY;
}

void ContinuousRenderer::on_task_(void *arg) {
  auto self = static_cast<ContinuousRenderer *>(arg);
  self->do_task_();
}

void ContinuousRenderer::do_task_() {
  for (;;) {
    xTaskNotifyWait(0, 0, NULL, portMAX_DELAY);
    xSemaphoreTake(this->mutex_handle_, portMAX_DELAY);

    if (this->render_with_mutex_() == LedStripStatus::ERROR)
      this->render_error_ = true;

    if (this->timer_running_ && (this->render_error_ || this->done_)) {
      esp_timer_stop(this->timer_handle_);
      this->timer_running_ = false;
    }

    xSemaphoreGive(this->mutex_handle_);
  }
}

void ContinuousRenderer::on_timer_(void *arg) {
  auto self = static_cast<ContinuousRenderer *>(arg);
  BaseType_t higher_priority_task_woken{};
  xTaskNotifyFromISR(self->task_handle_, 0, eNoAction, &higher_priority_task_woken);
  if (higher_priority_task_woken)
    esp_timer_isr_dispatch_need_yield();
}

bool OneBufferContinuousRenderer::setup() {
  RAMAllocator<uint8_t> allocator;
  this->color_data_ = allocator.allocate(this->color_data_bytes());
  if (this->color_data_ == nullptr)
    return false;
  return ContinuousRenderer::setup();
}

void OneBufferContinuousRenderer::copy_with_mutex_(const uint8_t *color_data, uq24_8_t scale) {
  memcpy(this->color_data_, color_data, this->color_data_bytes());
  this->scale_ = scale;
}

LedStripStatus OneBufferContinuousRenderer::render_with_mutex_() {
  return this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_pixels,
                                 InputOpFast{.color_data = this->color_data_}, this->scale_);
}

#endif  // USE_LED_STRIP_CONTINUOUS_RENDERER

}  // namespace esphome::led_strip::renderer
