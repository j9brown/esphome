#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "esphome/core/defines.h"
#include "esphome/core/helpers.h"
#include "led_strip.h"

#ifdef USE_LED_STRIP_CONTINUOUS_RENDERER
#include "esp_timer.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#endif

namespace esphome::led_strip::renderer {

// Unsigned fixed point integer types of various sizes.
//
// LED rendering uses fixed point numbers in the range of 0.0 to 255.0 to represent the intensity of
// each color channel.  This representation allows for efficient scaling of intensities using only
// multiplication, addition, and bit-shifting without division or saturating addition.  When transferring
// intensities to the hardware, the fractional bits are simply truncated.
//
// In contrast, if a non-fractional intermediate representation were used instead (such as 16-bit intensity) then
// more expensive operations would be needed to preserve the full range when converting to and from an 8-bit
// representation (such as r16 = r8 * 65535 / 255).
using uq24_8_t = uint32_t;   // Q24.8
using uq16_16_t = uint32_t;  // Q16.16
using uq8_24_t = uint32_t;   // Q8.24
using uq8_8_t = uint16_t;    // Q8.8

/// Scales the colors that are read from the input color data.
/// Each factor must be between 0.0 and 1.0 (0x100 in binary representation).
struct ColorScale {
  uq24_8_t r{0x100};
  uq24_8_t g{0x100};
  uq24_8_t b{0x100};
  uq24_8_t w{0x100};
};

struct InputOpFast {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next(uq24_8_t scale) { return *(this->color_data++) * scale; }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_r() { return this->read_next(this->color_scale.r); }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_g() { return this->read_next(this->color_scale.g); }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_b() { return this->read_next(this->color_scale.b); }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_w() { return this->read_next(this->color_scale.w); }

  const uint8_t *color_data;
  const ColorScale color_scale;
};

struct InputOpLerp {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next(uq24_8_t scale_a, uq24_8_t scale_b) {
    const uq24_8_t value_a = *(this->color_data_a++) * scale_a;
    const uq24_8_t value_b = *(this->color_data_b++) * scale_b;
    return (value_a + (((value_b - value_a) * this->alpha) >> 8)) & 0xffff;  // arithmetic may wrap
  }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_r() {
    return this->read_next(this->color_scale_a.r, this->color_scale_b.r);
  }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_g() {
    return this->read_next(this->color_scale_a.g, this->color_scale_b.g);
  }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_b() {
    return this->read_next(this->color_scale_a.b, this->color_scale_b.b);
  }
  inline ESPHOME_ALWAYS_INLINE uq24_8_t read_next_w() {
    return this->read_next(this->color_scale_a.w, this->color_scale_b.w);
  }

  const uint8_t *color_data_a;
  const uint8_t *color_data_b;
  const ColorScale color_scale_a;
  const ColorScale color_scale_b;
  const uq24_8_t alpha;
};

// Refer to `_generate_lut` in the code generator for details on the contents.
struct Lut {
  uq8_8_t entries[257];
};

struct LutOpNone {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t operator()(uq24_8_t value) const { return value; }
};

struct LutOpFast {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t operator()(uq24_8_t value) const {
    const uint32_t lut_index = value >> 8;
    return this->lut->entries[lut_index];
  }

  const Lut *const lut;
};

struct LutOpLerp {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t operator()(uq24_8_t value) const {
    const uint32_t lut_index = value >> 8;
    const uq24_8_t lut_fraction = value & 0xff;
    const uq24_8_t lut_entry_low = lut->entries[lut_index];
    const uq24_8_t lut_entry_high = lut->entries[lut_index + 1];
    return lut_entry_low + (((lut_entry_high - lut_entry_low) * lut_fraction) >> 8);
  }

  const Lut *const lut;
};

struct DitherOpNone {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t operator()(uq24_8_t value) const { return value; }

  inline void next_pixel() {}
  inline void next_frame() {}
};

template<bool temporal> struct DitherOpPattern {
  inline ESPHOME_ALWAYS_INLINE uq24_8_t operator()(uq24_8_t value) const { return value + this->noise; }

  inline ESPHOME_ALWAYS_INLINE void next() {
    // Produces a bit reversal permutation of a given size as a dither noise pattern.
    // It has the nice pattern of spreading out terms with similar error.
    // - 0, 1
    // - 0, 2, 1, 3
    // - 0, 4, 2, 6, 1, 5, 3, 7
    this->sequence = (this->sequence + 1) & ((1u << this->dither_bits) - 1u);
    this->noise = reverse_bits(this->sequence);
  }

  inline ESPHOME_ALWAYS_INLINE void next_pixel() {
    if constexpr (!temporal) {
      this->next();
    }
  }

  inline ESPHOME_ALWAYS_INLINE void next_frame() {
    if constexpr (temporal) {
      this->next();
    } else {
      sequence = 0;
      noise = 0;
    }
  }

  const uint8_t dither_bits;
  uint8_t sequence{0};
  uint8_t noise{0};
};

using DitherOpSpatial = DitherOpPattern<false>;
using DitherOpTemporal = DitherOpPattern<true>;

template<typename LutOp, typename DitherOp> struct ChannelOp {
  inline ESPHOME_ALWAYS_INLINE uint8_t operator()(uq24_8_t value) const {
    return this->dither_op(this->lut_op(value)) >> 8;
  }

  LutOp lut_op;
  DitherOp dither_op;
};

template<typename ChannelOp> struct RenderOpRGB {
  template<typename InputOp, typename OutputOp>
  inline ESPHOME_ALWAYS_INLINE void render(OutputOp &output_op, size_t num_leds, InputOp input_op) {
    for (size_t i = 0; i < num_leds; i++) {
      output_op.write_next_pixel(this->channel_op(input_op.read_next_r()), this->channel_op(input_op.read_next_g()),
                                 this->channel_op(input_op.read_next_b()));
      channel_op.dither_op.next_pixel();
    }
    channel_op.dither_op.next_frame();
  }

  ChannelOp channel_op;
};

template<typename ChannelOp> struct RenderOpRGBW {
  template<typename InputOp, typename OutputOp>
  inline ESPHOME_ALWAYS_INLINE void render(OutputOp &output_op, size_t num_leds, InputOp input_op) {
    for (size_t i = 0; i < num_leds; i++) {
      output_op.write_next_pixel(this->channel_op(input_op.read_next_r()), this->channel_op(input_op.read_next_g()),
                                 this->channel_op(input_op.read_next_b()), this->channel_op(input_op.read_next_w()));
      channel_op.dither_op.next_pixel();
    }
    channel_op.dither_op.next_frame();
  }

  ChannelOp channel_op;
};

template<typename InputOp> struct PipelineBase {
  virtual LedStripStatus render(LedStrip *led_strip, size_t num_leds, InputOp input_op) = 0;
};

template<typename InputOp, typename RenderOp, typename OutputOp> struct Pipeline final : public PipelineBase<InputOp> {
  explicit Pipeline(RenderOp render_op) : render_op(std::move(render_op)) {}

  LedStripStatus render(LedStrip *led_strip, size_t num_leds, InputOp input_op) override {
    alignas(OutputOp) std::byte output_op_storage[sizeof(OutputOp)];

    LedStripStatus status = led_strip->begin_frame(output_op_storage);
    if (status == LedStripStatus::READY) {
      auto &output_op = *std::launder(reinterpret_cast<OutputOp *>(output_op_storage));
      this->render_op.render(output_op, num_leds, input_op);
      led_strip->end_frame(&output_op);
    }
    return status;
  }

  RenderOp render_op;
};

/// Describes immutable characteristics of an LED strip renderer.
struct RendererTraits {
  /// True if the renderer supports interpolating transitions between frames.
  bool supports_transitions;
};

/// Describes a request to show new content.
struct RendererShow {
  /// RGB or RGBW color data.
  const uint8_t *color_data;
  /// Prescaling factors to apply to the color data while reading from it.
  const ColorScale &color_scale;
  /// The number of microseconds to interpolate the transition from the previous content
  /// to the new content when the renderer supports that feature (as described in its traits).
  /// Set to zero for no transition.
  uint32_t transition_us;
  /// When true, tells the renderer to stop rendering new in-between frames after the transition
  /// completes and this content is shown.
  bool done;
};

/// Writes pixels to an LED Strip after applying a pipeline of render operations
/// for linear interpolation, color correction, brightness scaling, and/or dithering.
class Renderer {
 public:
  explicit Renderer(LedStrip *led_strip, RendererTraits renderer_traits);

  inline const LedStripTraits &led_strip_traits() const { return this->led_strip_traits_; }
  inline const RendererTraits &renderer_traits() const { return this->renderer_traits_; }
  inline size_t color_data_bytes() const { return this->color_data_bytes_; }

#ifdef USE_LED_STRIP_FRAME_STATS
  void set_frame_stats_report_interval_us(uint32_t value_us) { this->frame_stats_report_interval_us_ = value_us; }
#endif

  /// Called from the main loop to dump the config.
  virtual void dump_config() = 0;

  /// Called from the main loop to setup the renderer.
  /// Returns true on success, false if an error occurred.
  virtual bool setup();

  /// Called from the main loop to show new color data.
  /// Returns status about the LED strip.
  virtual LedStripStatus show(const RendererShow &show) = 0;

 protected:
  LedStrip *const led_strip_;
  const LedStripTraits led_strip_traits_;
  const RendererTraits renderer_traits_;
  const size_t color_data_bytes_;

#ifdef USE_LED_STRIP_FRAME_STATS
  uint32_t frame_stats_report_interval_us_{};  // constant after initialization
#endif
};

/// Renders frames on the main loop.
/// Not suitable for dithering or interpolation.
class ImmediateRenderer final : public Renderer {
 public:
  ImmediateRenderer(LedStrip *led_strip, PipelineBase<InputOpFast> *pipeline)
      : Renderer(led_strip, {}), pipeline_(pipeline) {}

  void dump_config() override;
  LedStripStatus show(const RendererShow &show) override;

 protected:
  PipelineBase<InputOpFast> *const pipeline_;

#ifdef USE_LED_STRIP_FRAME_STATS
  struct FrameStats {
    uint32_t report_start_us{};
    uint32_t slowest_us{};
    uint32_t fastest_us{UINT32_MAX};
    uint32_t total_us{};
    uint32_t last_frame_us{};
    uint32_t frame_period_sum_us{};
    uint16_t frame_count{};
    uint16_t busy_count{};
  } frame_stats_;
#endif
};

#ifdef USE_LED_STRIP_CONTINUOUS_RENDERER

/// Renders pixels continuously from a real-time task driven by a timer at a consistent frame rate.
class ContinuousRenderer : public Renderer {
 public:
  ContinuousRenderer(LedStrip *led_strip, unsigned frame_rate, bool supports_transitions)
      : Renderer(led_strip, {.supports_transitions = supports_transitions}), frame_period_us_(1000000u / frame_rate) {}

#ifdef USE_LED_STRIP_FRAME_STATS
  void set_frame_stats_report_interval_us(uint32_t value_us) { this->frame_stats_report_interval_us_ = value_us; }
#endif

  void dump_config() override;
  bool setup() override;
  LedStripStatus show(const RendererShow &show) override;

 protected:
  struct RenderOutputResult {
    LedStripStatus led_strip_status;
    bool transition_in_progress;
  };

  static void on_task_(void *arg);
  void do_task_();

  static void on_timer_(void *arg);

  virtual void copy_input_m_(const RendererShow &show) = 0;
  virtual RenderOutputResult render_output_m_() = 0;

  const uint32_t frame_period_us_;

  esp_timer_handle_t timer_handle_{};  // start/stop guarded by mutex
  SemaphoreHandle_t mutex_handle_{};
  TaskHandle_t task_handle_{};

  bool done_m_{};           // guarded by mutex, set to true when the timer should be stopped
  bool timer_running_m_{};  // guarded by mutex
  bool render_error_m_{};   // guarded by mutex

#ifdef USE_LED_STRIP_FRAME_STATS
  struct FrameStatsGuarded {
    uint32_t report_start_us{};
    uint32_t last_input_us{};
    uint32_t input_period_sum_us{};
    uint16_t input_count{};
  } frame_stats_m_;  // guarded by mutex
  struct FrameStats {
    uint32_t slowest_us{};
    uint32_t fastest_us{UINT32_MAX};
    uint32_t total_us{};
    uint16_t frame_count{};
    uint16_t busy_count{};
    uint16_t lost_count{};
  } frame_stats_;  // NOT guarded by mutex (only accessed on task)
#endif
};

class FastContinuousRenderer final : public ContinuousRenderer {
 public:
  FastContinuousRenderer(LedStrip *led_strip, unsigned frame_rate, PipelineBase<InputOpFast> *pipeline)
      : ContinuousRenderer(led_strip, frame_rate, false), pipeline_(pipeline) {}

  bool setup() override;

 protected:
  void copy_input_m_(const RendererShow &show) override;
  RenderOutputResult render_output_m_() override;

  PipelineBase<InputOpFast> *const pipeline_;

  uint8_t *color_data_{};
  ColorScale color_scale_{};
};

class LerpContinuousRenderer final : public ContinuousRenderer {
 public:
  LerpContinuousRenderer(LedStrip *led_strip, unsigned frame_rate, PipelineBase<InputOpLerp> *pipeline)
      : ContinuousRenderer(led_strip, frame_rate, true), pipeline_(pipeline) {}

  bool setup() override;

 protected:
  void copy_input_m_(const RendererShow &show) override;
  RenderOutputResult render_output_m_() override;

  PipelineBase<InputOpLerp> *const pipeline_;

  uint8_t *color_data_[2]{};
  ColorScale color_scales_[2]{};
  uint32_t transition_end_us_{};
  uint32_t transition_duration_us_{};
  uint8_t front_buffer_{};
};

#endif

}  // namespace esphome::led_strip::renderer
