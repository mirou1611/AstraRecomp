#include "ps2vita/gs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace ps2vita {
namespace {

std::int64_t edge(const GsVertex& a, const GsVertex& b, int x, int y) {
  return static_cast<std::int64_t>(x - a.x) * (b.y - a.y) -
         static_cast<std::int64_t>(y - a.y) * (b.x - a.x);
}

std::uint32_t mix_channel(std::uint32_t ca, std::uint32_t cb, std::uint32_t cc,
                          std::int64_t wa, std::int64_t wb, std::int64_t wc,
                          std::int64_t area, unsigned shift) {
  const auto a = static_cast<std::int64_t>((ca >> shift) & 0xFFu);
  const auto b = static_cast<std::int64_t>((cb >> shift) & 0xFFu);
  const auto c = static_cast<std::int64_t>((cc >> shift) & 0xFFu);
  const auto value = (a * wa + b * wb + c * wc) / area;
  return static_cast<std::uint32_t>(std::clamp<std::int64_t>(value, 0, 255)) << shift;
}

} // namespace

Gs::Gs() : color_(kWidth * kHeight), depth_(kWidth * kHeight) { clear(0); }

void Gs::clear(std::uint32_t color, std::uint32_t depth) {
  std::fill(color_.begin(), color_.end(), color);
  std::fill(depth_.begin(), depth_.end(), depth);
}

void Gs::write(int x, int y, std::uint32_t z, std::uint32_t color) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  if (x < scissor_left_ || x > scissor_right_ ||
      y < scissor_top_ || y > scissor_bottom_) return;
  const auto index = static_cast<std::size_t>(y * kWidth + x);
  const bool pass = depth_test_ == DepthTest::Always ||
      (depth_test_ == DepthTest::GreaterEqual && z >= depth_[index]) ||
      (depth_test_ == DepthTest::Greater && z > depth_[index]) ||
      (depth_test_ == DepthTest::LessEqual && z <= depth_[index]);
  if (pass) {
    if (depth_write_) depth_[index] = z;
    if (blend_enabled_ && (!blend_pabe_ || (color & 0x80000000u) != 0u)) {
      const auto destination = color_[index];
      const auto source = color;
      const unsigned a = blend_equation_ & 3u;
      const unsigned b = (blend_equation_ >> 2) & 3u;
      const unsigned c = (blend_equation_ >> 4) & 3u;
      const unsigned d = (blend_equation_ >> 6) & 3u;
      // Selectors 3 are reserved; only the defined equation subset is modeled.
      if (a < 3u && b < 3u && c < 3u && d < 3u) {
        const int alpha = c == 0u ? source >> 24 : c == 1u ? destination >> 24 :
            (blend_equation_ >> 32) & 0xFFu;
        color &= 0xFF000000u; // Blending modifies RGB, not source alpha.
        for (unsigned shift = 0; shift < 24u; shift += 8u) {
          const int components[] = {int((source >> shift) & 0xFFu),
                                    int((destination >> shift) & 0xFFu), 0};
          const int product = (components[a] - components[b]) * alpha;
          const int scaled = product >= 0 ? product / 128 : -((-product + 127) / 128);
          const int value = scaled + components[d];
          const auto channel = color_clamp_ ? unsigned(std::clamp(value, 0, 255)) :
              static_cast<unsigned>(value) & 0xFFu;
          color |= channel << shift;
        }
      }
    }
    color_[index] = color;
  }
}

void Gs::point(const GsVertex& vertex) {
  write(vertex.x, vertex.y, vertex.z, vertex.color);
}

void Gs::line(GsVertex a, GsVertex b) {
  const int dx = std::abs(b.x - a.x);
  const int dy = std::abs(b.y - a.y);
  const int steps = std::max(dx, dy);
  if (steps == 0) { point(a); return; }
  for (int i = 0; i <= steps; ++i) {
    const auto lerp = [=](std::uint32_t av, std::uint32_t bv) {
      return static_cast<std::uint32_t>((static_cast<std::uint64_t>(av) * (steps - i) +
                                         static_cast<std::uint64_t>(bv) * i) / steps);
    };
    std::uint32_t color = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
      color |= lerp((a.color >> shift) & 0xFFu, (b.color >> shift) & 0xFFu) << shift;
    write(a.x + (b.x - a.x) * i / steps, a.y + (b.y - a.y) * i / steps,
          lerp(a.z, b.z), color);
  }
}

void Gs::triangle(GsVertex a, GsVertex b, GsVertex c) {
  std::int64_t area = edge(a, b, c.x, c.y);
  // A collapsed triangle has no coverage; it is not a line primitive.
  if (area == 0) return;
  if (area < 0) { std::swap(b, c); area = -area; }
  const int min_x = std::max(0, std::min({a.x, b.x, c.x}));
  const int max_x = std::min(kWidth - 1, std::max({a.x, b.x, c.x}));
  const int min_y = std::max(0, std::min({a.y, b.y, c.y}));
  const int max_y = std::min(kHeight - 1, std::max({a.y, b.y, c.y}));
  for (int y = min_y; y <= max_y; ++y) {
    for (int x = min_x; x <= max_x; ++x) {
      const auto wa = edge(b, c, x, y);
      const auto wb = edge(c, a, x, y);
      const auto wc = edge(a, b, x, y);
      if (wa < 0 || wb < 0 || wc < 0) continue;
      const auto z = static_cast<std::uint32_t>((
          static_cast<std::uint64_t>(a.z) * wa +
          static_cast<std::uint64_t>(b.z) * wb +
          static_cast<std::uint64_t>(c.z) * wc) / static_cast<std::uint64_t>(area));
      std::uint32_t color = 0;
      for (unsigned shift = 0; shift < 32; shift += 8)
        color |= mix_channel(a.color, b.color, c.color, wa, wb, wc, area, shift);
      write(x, y, z, color);
    }
  }
}

std::uint32_t Gs::pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return 0;
  return color_[static_cast<std::size_t>(y * kWidth + x)];
}

} // namespace ps2vita
