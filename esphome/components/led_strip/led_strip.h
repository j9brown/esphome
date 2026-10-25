#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::led_strip {
namespace renderer {

/// The output operation is used by the renderer to write pixel data into the frame buffer of an `LedStrip`.
/// It defines a single method called `write_next_pixel` which accepts RGB or RGBW color channel values and
/// advances the write offset by one pixel (much like an output iterator).
///
/// This header defines output operations for a few common representations.  Components can also define
/// custom output operations to match other representations and optimize data transfer.  During code
/// generation, the LED strip component registers the type name of the `OutputOp` subclass that the
/// renderer should expect to receive from the component at runtime given how the component has been
/// configured.
///
/// An efficient output operation should contain no branches.
struct OutputOp {};

struct LinearOutputOp : public OutputOp {
  uint8_t *data;
};

template<size_t bytes_per_pixel, size_t r_offset, size_t g_offset, size_t b_offset>
struct LinearOutputOpRGB : public LinearOutputOp {
  inline void write_next_pixel(uint8_t r, uint8_t g, uint8_t b) {
    this->data[r_offset] = r;
    this->data[g_offset] = g;
    this->data[b_offset] = b;
    this->data += bytes_per_pixel;
  }
};

template<size_t bytes_per_pixel, size_t r_offset, size_t g_offset, size_t b_offset, size_t w_offset>
struct LinearOutputOpRGBW : public LinearOutputOp {
  inline void write_next_pixel(uint8_t r, uint8_t g, uint8_t b, uint8_t w) {
    this->data[r_offset] = r;
    this->data[g_offset] = g;
    this->data[b_offset] = b;
    this->data[w_offset] = w;
    this->data += bytes_per_pixel;
  }
};

}  // namespace renderer

/// Describes immutable characteristics of an LED strip.
struct LedStripTraits {
  /// The number of individually addressable logical pixels in the LED strip.
  /// In some LED strips, multiple adjacent LEDs may show the same color and thus they are effectively part
  /// of the same pixel so the number of logical pixels may differ from the number of physical LEDs.
  size_t num_leds;

  /// True if the LED strip uses the white color channel.
  bool has_w_channel;
};

enum class LedStripStatus : uint8_t {
  /// The LED strip is ready to accept pixel data for the next frame.
  READY = 0,
  /// The LED strip is busy processing previous frames, try again later.
  BUSY = 1,
  /// The LED strip cannot accept pixel data due to an error, don't try again.
  ERROR = 2,
};

/// Interface for components that transfers pixel data to an LED strip.
class LedStrip {
 public:
  /// Called by the renderer to get immutable characteristics of this LED strip.
  virtual LedStripTraits get_traits() const = 0;

  /// Called by the renderer to begin writing the next frame.
  ///
  /// The renderer calls this function on the render thread so it must not block.
  ///
  /// The renderer performs the following steps to generate each frame:
  ///
  ///   1. Call `begin_frame()` to acquire an output operation, only proceed if it returns READY.
  ///   2. Call `write_next_pixel()` on the output operation exactly once for every pixel in the LED
  ///      strip as specified by `LedStripTraits::num_leds` to populate the frame in progress.
  ///   3. Call `end_frame()` to release the output operation and start transferring the frame in
  ///      progress to the hardware.
  ///
  /// The implementation can rely on these rendering invariants to optimize away unnecessary actions such
  /// as bounds checking and buffer clearing.
  ///
  /// The implementation of `begin_frame` should use the placement new operator to initialize an instance
  /// of the `OutputOp` subclass of the type specified in the led strip traits during code generation
  /// into the storage provided by the caller at `output_op`.
  virtual LedStripStatus begin_frame(void *output_op) = 0;

  /// Called by the renderer to start transferring the frame in progress to the hardware.
  ///
  /// The implementation should destroy the `output_op` object to complete its lifecycle (not needed
  /// when the `OutputOp` subclass is a plain old data type).
  ///
  /// This function is called on the render thread and it must not block.
  virtual void end_frame(renderer::OutputOp *output_op) = 0;
};

}  // namespace esphome::led_strip
