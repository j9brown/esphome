#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome::led_strip {

/// Represents a LED pixel's RGB(W) intensity after all rendering operations have been applied including
/// interpolation, scaling, color correction, and dithering.  Not to be confused with `esphome::Color`
/// which represents a linear color from the source of the rendering.
struct LedColor {
  uint8_t r;
  uint8_t g;
  uint8_t b;
  uint8_t w;
};

/// Describes immutable characteristics of an LED strip.
struct LedStripTraits {
  /// The number of individually addressable logical pixels in the LED strip.
  /// In some LED strips, multiple adjacent LEDs may show the same color and thus they are effectively part
  /// of the same pixel so the number of logical pixels may differ from the number of physical LEDs.
  size_t num_pixels;

  /// True if the LED strip uses the white color channel.
  bool has_w_channel;

  /// The estimated time interval in microseconds between the moment when the renderer presents
  /// a new frame and the moment when the physical pixels begin to illuminate.
  // uint32_t latency_us;
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
  ///   1. Call `begin_frame()`, only proceed if it returns READY.
  ///   2. Call `push_pixel()` exactly once for every pixel in the LED strip as described
  ///      by `LedStripTraits::num_pixels` to populate the frame in progress.
  ///   3. Call `present_frame()` to start transferring the frame in progress to the hardware.
  ///
  /// The LED strip can rely on these rendering invariants to optimize away unnecessary actions such as
  /// bounds checking and buffer clearing.
  virtual LedStripStatus begin_frame() = 0;

  /// Called by the renderer to set the color of the next pixel in the LED strip for the frame in progress.
  ///
  /// The renderer calls this function on the render thread so it must not block.
  virtual void push_pixel(LedColor color) = 0;

  /// Called by the renderer to start transferring the frame in progress to the hardware.
  ///
  /// This function is called on the render thread and it must not block.
  virtual void present_frame() = 0;
};

}  // namespace esphome::led_strip
