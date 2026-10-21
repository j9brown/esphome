#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

#include "esphome/core/helpers.h"
#include "led_strip.h"

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

struct InputOpFast {
  inline uq24_8_t read_next_scaled_value(uq24_8_t scale) {
    const uint8_t value = *(this->color_data++);
    return value * scale;
  }

  const uint8_t *color_data;
};

struct InputOpLerp {
  inline uq24_8_t read_next_scaled_value(uq24_8_t scale) {
    const uint8_t value_a = *(this->color_data_a++);
    const uint8_t value_b = *(this->color_data_b++);
    const uq24_8_t value_lerp = ((value_a << 8) + (value_b - value_a) * this->alpha) & 0xffff;  // subtraction may wrap
    return (value_lerp * scale) >> 8;
  }

  const uint8_t *color_data_a;
  const uint8_t *color_data_b;
  const uq24_8_t alpha;
};

/// This look-up table transforms scaled linear RGB(W) intensities in the range of 0 to 255 to the non-linear
/// domain of an LED color by applying a non-decreasing monotonic function such as exponential gamma or some
/// other curve as appropriate for the hardware.
///
/// The 0th entry must be 0.
/// The 255th entry must be no larger than 1.0 (0xff00 in binary representation).
/// The 256th entry must equal the 255th entry to allow indexing one entry beyond the maximum value.
struct Lut {
  uq8_8_t entries[257];
};

struct LutOpNone {
  inline uq24_8_t operator()(uq24_8_t value) const { return value; }
};

struct LutOpFast {
  inline uq24_8_t operator()(uq24_8_t value) const {
    const uint32_t lut_index = value >> 8;
    return this->lut->entries[lut_index];
  }

  const Lut *const lut;
};

struct LutOpLerp {
  inline uq24_8_t operator()(uq24_8_t value) const {
    const uint32_t lut_index = value >> 8;
    const uq24_8_t lut_fraction = value & 0xff;
    const uq24_8_t lut_entry_low = lut->entries[lut_index];
    const uq24_8_t lut_entry_high = lut->entries[lut_index + 1];
    return lut_entry_low + (((lut_entry_high - lut_entry_low) * lut_fraction) >> 8);
  }

  const Lut *const lut;
};

struct DitherOpNone {
  inline uq24_8_t operator()(uq24_8_t value) const { return value; }

  inline void advance_pattern() {}
};

struct DitherOpFast {
  inline uq24_8_t operator()(uq24_8_t value) const { return value + this->noise; }

  inline void advance_pattern() {
    // Produces a bit reversal permutation of a given size as a dither noise pattern.
    // It has the nice pattern of spreading out terms with similar error.
    // - 0, 1
    // - 0, 2, 1, 3
    // - 0, 4, 2, 6, 1, 5, 3, 7
    this->sequence = (this->sequence + 1) & ((1u << this->dither_bits) - 1u);
    this->noise = reverse_bits(this->sequence);
  }

  const uint8_t dither_bits;
  uint8_t sequence{0};
  uint8_t noise{0};
};

template<typename LutOp, typename DitherOp> struct ChannelOp {
  inline uint8_t operator()(uq24_8_t value) const { return this->dither_op(this->lut_op(value)) >> 8; }

  LutOp lut_op;
  DitherOp dither_op;
};

template<typename ChannelOp> struct RenderOpRGB {
  template<typename InputOp, typename OutputOp>
  inline void render(OutputOp &output_op, size_t num_pixels, InputOp input_op, uq24_8_t scale) const {
    for (size_t i = 0; i < num_pixels; i++) {
      output_op.write_next_pixel(this->channel_op(input_op.read_next_scaled_value(scale)),
                                 this->channel_op(input_op.read_next_scaled_value(scale)),
                                 this->channel_op(input_op.read_next_scaled_value(scale)));
    }
  }

  ChannelOp channel_op;
};

template<typename ChannelOp> struct RenderOpRGBCorrected {
  template<typename InputOp, typename OutputOp>
  inline void render(OutputOp &output_op, size_t num_pixels, InputOp input_op, uq24_8_t scale) const {
    const uq24_8_t r_scale = (this->r_correction * scale) >> 8;
    const uq24_8_t g_scale = (this->g_correction * scale) >> 8;
    const uq24_8_t b_scale = (this->b_correction * scale) >> 8;
    for (size_t i = 0; i < num_pixels; i++) {
      output_op.write_next_pixel(this->channel_op(input_op.read_next_scaled_value(r_scale)),
                                 this->channel_op(input_op.read_next_scaled_value(g_scale)),
                                 this->channel_op(input_op.read_next_scaled_value(b_scale)));
    }
  }

  /// The color correction factors must be between 0 and 1.0 (0x100 in binary representation).
  const uq8_8_t r_correction{0x100};
  const uq8_8_t g_correction{0x100};
  const uq8_8_t b_correction{0x100};

  ChannelOp channel_op;
};

template<typename ChannelOp> struct RenderOpRGBW {
  template<typename InputOp, typename OutputOp>
  inline void render(OutputOp &output_op, size_t num_pixels, InputOp input_op, uq24_8_t scale) const {
    for (size_t i = 0; i < num_pixels; i++) {
      output_op.write_next_pixel(this->channel_op(input_op.read_next_scaled_value(scale)),
                                 this->channel_op(input_op.read_next_scaled_value(scale)),
                                 this->channel_op(input_op.read_next_scaled_value(scale)),
                                 this->channel_op(input_op.read_next_scaled_value(scale)));
    }
  }

  ChannelOp channel_op;
};

template<typename ChannelOp> struct RenderOpRGBWCorrected {
  template<typename InputOp, typename OutputOp>
  inline void render(OutputOp &output_op, size_t num_pixels, InputOp input_op, uq24_8_t scale) const {
    const uq24_8_t r_scale = (this->r_correction * scale) >> 8;
    const uq24_8_t g_scale = (this->g_correction * scale) >> 8;
    const uq24_8_t b_scale = (this->b_correction * scale) >> 8;
    const uq24_8_t w_scale = (this->w_correction * scale) >> 8;
    for (size_t i = 0; i < num_pixels; i++) {
      output_op.write_next_pixel(this->channel_op(input_op.read_next_scaled_value(r_scale)),
                                 this->channel_op(input_op.read_next_scaled_value(g_scale)),
                                 this->channel_op(input_op.read_next_scaled_value(b_scale)),
                                 this->channel_op(input_op.read_next_scaled_value(w_scale)));
    }
  }

  /// The color correction factors must be between 0 and 1.0 (0x100 in binary representation).
  const uq8_8_t r_correction{0x100};
  const uq8_8_t g_correction{0x100};
  const uq8_8_t b_correction{0x100};
  const uq8_8_t w_correction{0x100};

  ChannelOp channel_op;
};

template<typename InputOp> struct PipelineBase {
  virtual LedStripStatus render(LedStrip *led_strip, size_t num_pixels, InputOp input_op, uq24_8_t scale) = 0;
};

template<typename InputOp, typename RenderOp, typename OutputOp> struct Pipeline final : public PipelineBase<InputOp> {
  explicit Pipeline(RenderOp render_op) : render_op(std::move(render_op)) {}

  LedStripStatus render(LedStrip *led_strip, size_t num_pixels, InputOp input_op, uq24_8_t scale) override {
    alignas(OutputOp) std::byte output_op_storage[sizeof(OutputOp)];

    LedStripStatus status = led_strip->begin_frame(output_op_storage);
    if (status == LedStripStatus::READY) {
      auto &output_op = *std::launder(reinterpret_cast<OutputOp *>(output_op_storage));

      this->render_op.render(output_op, num_pixels, input_op, scale);
      this->render_op.channel_op.dither_op.advance_pattern();

      led_strip->end_frame(&output_op);
    }
    return status;
  }

  RenderOp render_op;
};

/// Writes pixels to an LED Strip after applying a pipeline of render operations
/// for linear interpolation, color correction, brightness scaling, and/or dithering.
class Renderer {
 public:
  explicit Renderer(LedStrip *led_strip);

  inline const LedStripTraits &traits() const { return this->led_strip_traits_; }
  inline size_t color_data_bytes() const { return this->color_data_bytes_; }

  /// Called from the main loop to dump the config.
  virtual void dump_config();

  /// Called from the main loop to setup the renderer.
  /// Returns true on success, false if an error occurred.
  virtual bool setup();

  /// Called from the main loop to show new color data.
  virtual LedStripStatus show(const uint8_t *color_data, uq24_8_t scale) = 0;

 protected:
  LedStrip *const led_strip_;
  const LedStripTraits led_strip_traits_;
  const size_t color_data_bytes_;
};

/// Renders frames on the main loop.
/// Not suitable for dithering or interpolation.
class ImmediateRenderer final : public Renderer {
 public:
  ImmediateRenderer(LedStrip *led_strip, PipelineBase<InputOpFast> *pipeline)
      : Renderer(led_strip), pipeline_(pipeline) {}

  LedStripStatus show(const uint8_t *color_data, uq24_8_t scale) override;

 protected:
  PipelineBase<InputOpFast> *const pipeline_;
};

/*
/// Renders pixels continuously from a real-time task driven by a timer at a consistent frame rate.
class ContinuousRenderer : public Renderer {
  public:
    ContinuousRenderer(LedStripLight* parent) : Renderer(parent) {}

    void show_from_main_loop() override;
};

/// Implements dithering only.
class OneBufferContinuousRenderer final : public ContinuousRenderer {
  public:
    OneBufferContinuousRenderer(LedStripLight* parent) : Renderer(parent) {}
};

/// Implements interpolation and dithering.
class TwoBufferContinuousRenderer final : public ContinuousRenderer {
  public:
    TwoBufferContinuousRenderer(LedStripLight* parent) : Renderer(parent) {}
};
*/

}  // namespace esphome::led_strip::renderer
