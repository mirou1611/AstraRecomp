#pragma once

#include <cstdint>
#include <vector>

namespace ps2vita {

struct GsVertex {
  int x = 0;
  int y = 0;
  std::uint32_t z = 0;
  std::uint32_t color = 0xFFFFFFFFu; // AABBGGRR, matching the Vita framebuffer.
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
  void clear(std::uint32_t color, std::uint32_t depth = 0xFFFFFFFFu);
  void point(const GsVertex& vertex);
  void line(GsVertex a, GsVertex b);
  void triangle(GsVertex a, GsVertex b, GsVertex c);

  const std::uint32_t* pixels() const { return color_.data(); }
  std::uint32_t pixel(int x, int y) const;

private:
  void write(int x, int y, std::uint32_t z, std::uint32_t color);
  std::vector<std::uint32_t> color_;
  std::vector<std::uint32_t> depth_;
  DepthTest depth_test_ = DepthTest::LessEqual; // Standalone host drawing.
  bool depth_write_ = true;
};

} // namespace ps2vita
