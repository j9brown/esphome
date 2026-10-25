#include "led_strip_renderer.h"

#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome::led_strip::renderer {

ESPHOME_LOG_TAG(TAG, "led_strip");

#ifdef USE_LED_STRIP_CONTINUOUS_RENDERER
constexpr const char TASK_NAME[] = "led_strip";
constexpr size_t TASK_STACK_SIZE = 4096;
constexpr ssize_t TASK_PRIORITY = 10;
#endif

constexpr size_t calc_color_data_bytes(const LedStripTraits &traits) {
  return traits.num_leds * (traits.has_w_channel ? 4 : 3);
}

Renderer::Renderer(LedStrip *led_strip, RendererTraits renderer_traits)
    : led_strip_(led_strip),
      led_strip_traits_(led_strip->get_traits()),
      renderer_traits_(renderer_traits),
      color_data_bytes_(calc_color_data_bytes(this->led_strip_traits_)) {}

void Renderer::dump_config() {}

bool Renderer::setup() { return true; }

void ImmediateRenderer::dump_config() {
  this->Renderer::dump_config();
  ESP_LOGCONFIG(TAG, "  Renderer: Immediate");
}

LedStripStatus ImmediateRenderer::show(const RendererShow &show) {
#ifdef USE_LED_STRIP_FRAME_STATS
  const uint32_t start_us = micros();
#endif

  const LedStripStatus status =
      this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_leds,
                              InputOpFast{.color_data = show.color_data, .color_scale = show.color_scale});

#ifdef USE_LED_STRIP_FRAME_STATS
  switch (status) {
    case LedStripStatus::READY: {
      const uint32_t frame_duration_us = micros() - start_us;
      if (frame_duration_us < this->frame_stats_.fastest_us)
        this->frame_stats_.fastest_us = frame_duration_us;
      if (frame_duration_us > this->frame_stats_.slowest_us)
        this->frame_stats_.slowest_us = frame_duration_us;
      this->frame_stats_.frame_count += 1;
      if (this->frame_stats_.frame_count > 1) {
        const uint32_t frame_period_us = start_us - this->frame_stats_.last_frame_us;
        this->frame_stats_.frame_period_sum_us += frame_period_us;
      }
      this->frame_stats_.last_frame_us = start_us;
      break;
    }
    case LedStripStatus::BUSY:
      this->frame_stats_.busy_count += 1;
      break;
    default:
      break;
  }
  if (this->frame_stats_.report_start_us == 0) {
    this->frame_stats_.report_start_us = start_us | 1u;  // ensure the value is non-zero
    App.scheduler.set_timeout(nullptr, "led_strip", this->frame_stats_report_interval_us_ / 1000, [this] {
      const uint32_t report_duration_us = (micros() | 1u) - this->frame_stats_.report_start_us;
      const float fps =
          (report_duration_us != 0 ? float(this->frame_stats_.frame_count * 1000000) / report_duration_us : 0);
      ESP_LOGI(TAG,
               "Frame stats: %.1f fps, %" PRIu16 " frames in %" PRIu32 " us with average period of %" PRIu32
               " us, slowest %" PRIu32 " us, fastest %" PRIu32 " us, %" PRIu16 " busy",
               fps, this->frame_stats_.frame_count, report_duration_us,
               this->frame_stats_.frame_count > 1
                   ? this->frame_stats_.frame_period_sum_us / (this->frame_stats_.frame_count - 1)
                   : 0,
               this->frame_stats_.slowest_us, this->frame_stats_.fastest_us, this->frame_stats_.busy_count);
      this->frame_stats_ = {};
    });
  }
#endif
  return status;
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

void ContinuousRenderer::dump_config() {
  this->Renderer::dump_config();
  ESP_LOGCONFIG(TAG, "  Renderer: Continuous\n  Frame rate: %.1f Hz", 1000000.f / this->frame_period_us_);
}

LedStripStatus ContinuousRenderer::show(const RendererShow &show) {
  xSemaphoreTake(this->mutex_handle_, portMAX_DELAY);

  const bool render_error = this->render_error_m_;
  bool notify = false;
  if (!render_error) {
    this->done_m_ = show.done;
#ifdef USE_LED_STRIP_FRAME_STATS
    const uint32_t input_us = micros();
    this->frame_stats_m_.input_count += 1;
    if (this->frame_stats_m_.input_count > 0) {
      const uint32_t input_period_us = input_us - this->frame_stats_m_.last_input_us;
      this->frame_stats_m_.input_period_sum_us += input_period_us;
    }
    this->frame_stats_m_.last_input_us = input_us;
#endif
    this->copy_input_m_(show);
    if (!this->timer_running_m_) {
      this->timer_running_m_ = true;
      esp_timer_start_periodic(this->timer_handle_, this->frame_period_us_);
      notify = true;
    }
  }

  xSemaphoreGive(this->mutex_handle_);
  if (notify)
    xTaskNotify(this->task_handle_, 0, eIncrement);
  return render_error ? LedStripStatus::ERROR : LedStripStatus::READY;
}

void ContinuousRenderer::on_task_(void *arg) {
  auto self = static_cast<ContinuousRenderer *>(arg);
  self->do_task_();
}

void ContinuousRenderer::do_task_() {
  for (;;) {
    uint32_t notify_count;
    xTaskNotifyWait(0, UINT32_MAX, &notify_count, portMAX_DELAY);
    xSemaphoreTake(this->mutex_handle_, portMAX_DELAY);

#ifdef USE_LED_STRIP_FRAME_STATS
    this->frame_stats_.lost_count += notify_count - 1;
    const uint32_t start_us = micros();
#endif

    RenderOutputResult result = this->render_output_m_();
    switch (result.led_strip_status) {
      case LedStripStatus::ERROR:
        this->render_error_m_ = true;
        break;
#ifdef USE_LED_STRIP_FRAME_STATS
      case LedStripStatus::READY: {
        const uint32_t frame_duration_us = micros() - start_us;
        if (frame_duration_us < this->frame_stats_.fastest_us)
          this->frame_stats_.fastest_us = frame_duration_us;
        if (frame_duration_us > this->frame_stats_.slowest_us)
          this->frame_stats_.slowest_us = frame_duration_us;
        this->frame_stats_.frame_count += 1;
        break;
      }
      case LedStripStatus::BUSY:
        this->frame_stats_.busy_count += 1;
        break;
#else
      default:
        break;
#endif
    }

    if (this->timer_running_m_ && (this->render_error_m_ || (this->done_m_ && !result.transition_in_progress))) {
      esp_timer_stop(this->timer_handle_);
      this->timer_running_m_ = false;
    }

#ifdef USE_LED_STRIP_FRAME_STATS
    const uint32_t report_interval_us = this->frame_stats_report_interval_us_;
    if (report_interval_us != 0) {
      const uint32_t report_us = start_us | 1u;  // ensure the value is non-zero
      uint32_t report_duration_us;
      bool do_report = false;
      if (this->frame_stats_m_.report_start_us == 0) {
        this->frame_stats_m_.report_start_us = report_us;
        report_duration_us = 0;
        do_report = true;
      } else {
        report_duration_us = report_us - this->frame_stats_m_.report_start_us;
        if (report_duration_us + this->frame_period_us_ >= report_interval_us) {
          this->frame_stats_m_.report_start_us = report_us;
          do_report = true;
        }
      }
      if (!this->timer_running_m_) {
        this->frame_stats_m_.report_start_us = 0;
        do_report = true;
      }
      if (do_report) {
        const uint16_t input_count = this->frame_stats_m_.input_count;
        const uint32_t input_period_sum_us = this->frame_stats_m_.input_period_sum_us;
        this->frame_stats_m_.input_count = 0;
        this->frame_stats_m_.input_period_sum_us = 0;
        xSemaphoreGive(this->mutex_handle_);

        const float fps =
            (report_duration_us != 0 ? float(this->frame_stats_.frame_count * 1000000) / report_duration_us : 0);
        ESP_LOGI(TAG,
                 "Frame stats: %.1f fps, %" PRIu16 " frames in %" PRIu32 " us, slowest %" PRIu32 " us, fastest %" PRIu32
                 " us, %" PRIu16 " busy, %" PRIu16 " lost; %" PRIu16 " inputs with average period of %" PRIu32 " us",
                 fps, this->frame_stats_.frame_count, report_duration_us, this->frame_stats_.slowest_us,
                 this->frame_stats_.fastest_us, this->frame_stats_.busy_count, this->frame_stats_.lost_count,
                 input_count, input_count > 1 ? input_period_sum_us / (input_count - 1) : 0);
        this->frame_stats_ = {};
        continue;  // already gave the semaphore away
      }
    }
#endif
    xSemaphoreGive(this->mutex_handle_);
  }
}

void ContinuousRenderer::on_timer_(void *arg) {
  auto self = static_cast<ContinuousRenderer *>(arg);
  BaseType_t higher_priority_task_woken{};
  xTaskNotifyFromISR(self->task_handle_, 0, eIncrement, &higher_priority_task_woken);
  if (higher_priority_task_woken)
    esp_timer_isr_dispatch_need_yield();
}

bool FastContinuousRenderer::setup() {
  RAMAllocator<uint8_t> allocator;
  this->color_data_ = allocator.allocate(this->color_data_bytes());
  if (this->color_data_ == nullptr)
    return false;
  return ContinuousRenderer::setup();
}

void FastContinuousRenderer::copy_input_m_(const RendererShow &show) {
  memcpy(this->color_data_, show.color_data, this->color_data_bytes());
  this->color_scale_ = show.color_scale;
}

ContinuousRenderer::RenderOutputResult FastContinuousRenderer::render_output_m_() {
  return {.led_strip_status =
              this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_leds,
                                      InputOpFast{.color_data = this->color_data_, .color_scale = this->color_scale_})};
}

bool LerpContinuousRenderer::setup() {
  const size_t bytes_per_buffer = this->color_data_bytes();
  RAMAllocator<uint8_t> allocator;
  this->color_data_[0] = allocator.allocate(bytes_per_buffer * 2);
  if (this->color_data_[0] == nullptr)
    return false;
  this->color_data_[1] = this->color_data_[0] + bytes_per_buffer;
  return ContinuousRenderer::setup();
}

void LerpContinuousRenderer::copy_input_m_(const RendererShow &show) {
  this->front_buffer_ ^= 1;
  const unsigned front_buffer = this->front_buffer_;
  memcpy(this->color_data_[front_buffer], show.color_data, this->color_data_bytes());
  this->color_scales_[front_buffer] = show.color_scale;
  if (show.transition_us != 0 && this->timer_running_m_) {
    this->transition_end_us_ = micros() + show.transition_us;
    this->transition_duration_us_ = show.transition_us;
  } else {
    // The timer will be started after returning from this method so if it's not running yet then that means the
    // light just turned on and only the front buffer will be valid so no transition is possible.
    this->transition_duration_us_ = 0;
  }
}

ContinuousRenderer::RenderOutputResult LerpContinuousRenderer::render_output_m_() {
  const unsigned front_buffer = this->front_buffer_;
  const unsigned back_buffer = front_buffer ^ 1;
  uq24_8_t alpha = 0;
  bool transition_in_progress = false;
  if (this->transition_duration_us_ != 0) {
    const uint32_t now_us = micros();
    if (now_us >= this->transition_end_us_) {
      this->transition_duration_us_ = 0;
    } else {
      alpha = (uint64_t(this->transition_end_us_ - now_us) << 8) / this->transition_duration_us_;
      transition_in_progress = true;
    }
  }
  return {.led_strip_status = this->pipeline_->render(this->led_strip_, this->led_strip_traits_.num_leds,
                                                      InputOpLerp{.color_data_a = this->color_data_[front_buffer],
                                                                  .color_data_b = this->color_data_[back_buffer],
                                                                  .color_scale_a = this->color_scales_[front_buffer],
                                                                  .color_scale_b = this->color_scales_[back_buffer],
                                                                  .alpha = alpha}),
          .transition_in_progress = transition_in_progress};
}

#endif  // USE_LED_STRIP_CONTINUOUS_RENDERER

}  // namespace esphome::led_strip::renderer
