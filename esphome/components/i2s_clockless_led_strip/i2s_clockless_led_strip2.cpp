#include "i2s_clockless_led_strip2.h"

#if defined(USE_ESP32) && SOC_I2S_SUPPORTS_TDM

#include <algorithm>
#include <cinttypes>

#include "esphome/core/helpers.h"

#include <driver/i2s_tdm.h>
#include <esp_attr.h>

namespace esphome::i2s_clockless_led_strip {

ESPHOME_LOG_TAG(TAG, "i2s_clockless_led_strip");

constexpr const char *const ERROR_ALLOCATION = "Allocation error";
constexpr const char *const ERROR_I2S = "I2S error";

void I2SClocklessLedStrip2::dump_config() {
  ESP_LOGCONFIG(TAG,
                "I2S Clockless LED Strip:\n"
                "  Pin: %u",
                this->pin_);
  char channel_colors[5];
  ESP_LOGCONFIG(TAG,
                "  Channel colors: %s\n"
                "  Number of LEDs: %" PRIu16 "\n"
                "  DMA buffer: %" PRIu16 " samples (%" PRIu16 " us)",
                this->channel_colors_.to_string(channel_colors), this->num_leds_, this->i2s_buffer_samples_,
                this->i2s_buffer_duration_us_);
}

float I2SClocklessLedStrip2::get_setup_priority() const { return setup_priority::IO; }

void I2SClocklessLedStrip2::setup() {
  RAMAllocator<uint8_t> allocator(RAMAllocator<uint8_t>::ALLOC_INTERNAL);
  this->i2s_data_ = allocator.allocate(this->i2s_data_bytes_);
  if (this->i2s_data_ == nullptr) {
    this->mark_failed(LOG_STR(ERROR_ALLOCATION));
    return;
  }
  memset(this->i2s_data_, 0, this->i2s_data_bytes_);

  i2s_chan_config_t chan_config = {
      .id = I2S_NUM_AUTO,
      .role = I2S_ROLE_MASTER,
      .dma_desc_num = 2,
      .dma_frame_num = this->i2s_buffer_samples_,
      .auto_clear_after_cb = false,
      .auto_clear_before_cb = false,
      .allow_pd = false,
      .intr_priority = 0,
  };
  esp_err_t err = i2s_new_channel(&chan_config, &this->tx_handle_, NULL);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "Error in i2s_new_channel: %s", esp_err_to_name(err));
  } else {
    i2s_tdm_config_t tdm_config = {
        .clk_cfg =
            {
                .sample_rate_hz = this->i2s_sample_rate_hz_,
                .clk_src = I2S_CLK_SRC_DEFAULT,
                .mclk_multiple = I2S_MCLK_MULTIPLE_384,
            },
        .slot_cfg =
            {
                .data_bit_width = I2S_DATA_BIT_WIDTH_24BIT,
                .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO,
                .slot_mode = I2S_SLOT_MODE_MONO,
                .slot_mask = I2S_TDM_SLOT0,
                .ws_width = 1,
                .big_endian = true,
                .total_slot = I2S_TDM_AUTO_SLOT_NUM,
            },
        .gpio_cfg =
            {
                .mclk = I2S_GPIO_UNUSED,
                .bclk = I2S_GPIO_UNUSED,
                .ws = I2S_GPIO_UNUSED,
                .dout = gpio_num_t(this->pin_),
                .din = I2S_GPIO_UNUSED,
                .invert_flags =
                    {
                        .mclk_inv = false,
                        .bclk_inv = false,
                        .ws_inv = false,
                    },
            },
    };
    err = i2s_channel_init_tdm_mode(this->tx_handle_, &tdm_config);
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "Error in i2s_channel_init_tdm_mode: %s", esp_err_to_name(err));
    } else {
      i2s_event_callbacks_t event_callbacks = {
          .on_sent = i2s_on_sent_callback,
      };
      err = i2s_channel_register_event_callback(this->tx_handle_, &event_callbacks, this);
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error in i2s_channel_register_event_callback: %s", esp_err_to_name(err));
      } else {
        err = i2s_channel_enable(this->tx_handle_);
        if (err == ESP_OK)
          return;  // Success!
        ESP_LOGE(TAG, "Error in i2s_channel_enable: %s", esp_err_to_name(err));
      }
    }
    i2s_del_channel(this->tx_handle_);
    this->tx_handle_ = {};
  }
  this->mark_failed(LOG_STR(ERROR_I2S));
}

led_strip::LedStripTraits I2SClocklessLedStrip2::get_traits() const {
  return led_strip::LedStripTraits{
      .num_leds = this->num_leds_,
      .has_w_channel = this->channel_colors_.has_white(),
  };
}

led_strip::LedStripStatus I2SClocklessLedStrip2::begin_frame(void *output_op) {
  if (this->is_failed())
    return led_strip::LedStripStatus::ERROR;

  if (this->i2s_data_ready_.load(std::memory_order_acquire))
    return led_strip::LedStripStatus::BUSY;

  new (output_op) renderer::I2SOutputOp{.i2s_data = this->i2s_data_};
  return led_strip::LedStripStatus::READY;
}

void I2SClocklessLedStrip2::end_frame(led_strip::renderer::OutputOp *output_op) {
  this->i2s_data_sent_ = 0;
  this->i2s_data_ready_.store(true, std::memory_order_release);
}

bool IRAM_ATTR HOT I2SClocklessLedStrip2::i2s_on_sent_callback(i2s_chan_handle_t handle, i2s_event_data_t *event,
                                                               void *user_ctx) {
  auto *const self = static_cast<I2SClocklessLedStrip2 *>(user_ctx);
  auto *const dma_buf = static_cast<uint8_t *>(event->dma_buf);
  const size_t dma_buf_size = event->size;

  bool dma_buf_filled = false;
  if (self->i2s_data_ready_.load(std::memory_order_acquire)) {
    if (self->i2s_data_sent_ < self->i2s_data_bytes_) {
      const size_t i2s_data_remaining = self->i2s_data_bytes_ - self->i2s_data_sent_;
      if (i2s_data_remaining >= dma_buf_size) {
        memcpy(dma_buf, self->i2s_data_ + self->i2s_data_sent_, dma_buf_size);
      } else {
        memcpy(dma_buf, self->i2s_data_ + self->i2s_data_sent_, i2s_data_remaining);
        memset(dma_buf + i2s_data_remaining, 0, dma_buf_size - i2s_data_remaining);
      }
      dma_buf_filled = true;
    }
    self->i2s_data_sent_ += dma_buf_size;
    if (self->i2s_data_sent_ >= self->i2s_data_bytes_ + self->i2s_reset_bytes_) {
      self->i2s_data_ready_.store(false, std::memory_order_release);
    }
  }
  // We only need to clear the buffer when the first byte is non-zero.  Zero is not a value
  // that can appear within the I2S data that gets copied into the front of the buffer so
  // if the first byte is zero then the buffer must have already been cleared.
  if (!dma_buf_filled && dma_buf[0] != 0) {
    memset(dma_buf, 0, dma_buf_size);
  }
  return false;
}

}  // namespace esphome::i2s_clockless_led_strip

#endif
