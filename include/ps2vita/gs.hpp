#pragma once

#include <cstdint>
#include <functional>
#include <vector>

namespace ps2vita {

struct GsVertex {
  int x = 0;
  int y = 0;
  std::uint32_t z = 0;
  std::uint32_t color = 0xFFFFFFFFu; // AABBGGRR, matching the Vita framebuffer.
  // Raw guest texture attributes, latched at XYZ kick (no host FP conversion).
  std::uint64_t st = 0, uv = 0;
  std::uint32_t q = 0;
};

enum class GsTracePrimitive { Unknown, Point, Line, Triangle, Sprite };
struct GsDrawTrace {
  GsTracePrimitive kind = GsTracePrimitive::Unknown;
  std::uint64_t sequence = 0, packet = 0;
  std::uint64_t prim = 0, frame = 0, tex0 = 0, clamp = 0;
  std::uint64_t alpha = 0, test = 0, texa = 0;
};
struct GsTextureTrace {
  bool valid = false;
  unsigned raw_u = 0, raw_v = 0, u = 0, v = 0, format = 0;
  std::uint32_t address = 0, raw_texel = 0, expanded_texel = 0;
  std::uint32_t vertex_color = 0, output_color = 0;
};
struct GsPixelWrite {
  std::uint32_t address = 0, before = 0, after = 0, input_color = 0, z = 0;
  int x = 0, y = 0;
  std::uint64_t frame = 0;
  GsDrawTrace draw{};
  GsTextureTrace texture{};
};

class Gs {
public:
  static constexpr int kWidth = 160;
  static constexpr int kHeight = 112;

  Gs();
  enum class DepthTest { Never, Always, GreaterEqual, Greater, LessEqual };
  void set_depth_state(DepthTest test, bool write) {
    depth_test_ = test;
    depth_write_ = write;
  }
  void set_scissor(int left, int top, int right, int bottom) {
    scissor_left_ = left; scissor_top_ = top;
    scissor_right_ = right; scissor_bottom_ = bottom;
  }
  void set_blend_state(bool enabled, std::uint64_t equation, bool pabe, bool clamp) {
    blend_enabled_ = enabled; blend_equation_ = equation;
    blend_pabe_ = pabe; color_clamp_ = clamp;
  }
  void set_alpha_test(std::uint64_t test) { alpha_test_ = test; }
  // Logical linear color storage, NOT native GS swizzled VRAM. Each covered
  // host sample represents a 4x4 native tile. Memory must outlive the binding.
  void set_color_target(std::vector<std::uint8_t>* memory, std::uint64_t frame);
  void clear(std::uint32_t color, std::uint32_t depth = 0xFFFFFFFFu);
  void point(const GsVertex& vertex);
  void line(GsVertex a, GsVertex b);
  using TextureSampler = std::function<std::uint32_t(unsigned, unsigned,
                                                     std::uint32_t)>;
  void triangle(GsVertex a, GsVertex b, GsVertex c,
                const TextureSampler& sample = {},
                unsigned st_width = 0, unsigned st_height = 0);

  // Preview of the current draw target, not privileged GS display scanout.
  const std::uint32_t* pixels() const;
  std::uint32_t pixel(int x, int y) const;
  std::uint64_t color_frame() const { return color_frame_; }

  // Opt-in diagnostic of accepted raster color operations in logical linear
  // VRAM. Watch physical byte addresses, so aliased FRAME targets are tracked.
  // Clear/IMAGE writes and rejected fragments are not reported. No callbacks
  // are made for the standalone host color buffer. Empty addresses disables.
  // Observers must not mutate/re-enter this Gs while a draw is in progress.
  using PixelObserver = std::function<void(const GsPixelWrite&)>;
  bool set_pixel_watch(const std::vector<std::uint32_t>& addresses,
                        PixelObserver observer = {});
  bool pixel_watch_enabled() const { return static_cast<bool>(pixel_observer_); }
  void set_draw_trace(const GsDrawTrace& trace) {
    if (pixel_watch_enabled()) { draw_trace_ = trace; texture_trace_ = {}; }
  }
  void set_texture_trace(const GsTextureTrace& trace) {
    if (pixel_watch_enabled()) texture_trace_ = trace;
  }

private:
  void write(int x, int y, std::uint32_t z, std::uint32_t color);
  std::uint32_t target_read(std::uint32_t address) const;
  void target_write(int x, int y, std::uint32_t color, std::uint32_t mask);
  mutable std::vector<std::uint32_t> color_;
  std::vector<std::uint8_t>* color_memory_ = nullptr;
  std::uint32_t color_base_ = 0, color_width_ = 0, color_mask_ = 0;
  std::uint64_t color_frame_ = 0;
  std::vector<std::uint32_t> pixel_watch_addresses_;
  PixelObserver pixel_observer_;
  GsDrawTrace draw_trace_{};
  GsTextureTrace texture_trace_{};
  std::vector<std::uint32_t> depth_;
  DepthTest depth_test_ = DepthTest::LessEqual; // Standalone host drawing.
  bool depth_write_ = true;
  bool blend_enabled_ = false, blend_pabe_ = false, color_clamp_ = true;
  std::uint64_t blend_equation_ = 0;
  std::uint64_t alpha_test_ = 0;
  int scissor_left_ = 0, scissor_top_ = 0;
  int scissor_right_ = kWidth - 1, scissor_bottom_ = kHeight - 1;
};

} // namespace ps2vita
