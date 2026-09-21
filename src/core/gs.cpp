#include "ps2vita/gs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <limits>

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

void Gs::set_color_target(std::vector<std::uint8_t>* memory, std::uint64_t frame) {
  const unsigned format = (frame >> 24) & 0x3Fu;
  color_width_ = ((frame >> 16) & 0x3Fu) * 64u;
  color_base_ = (frame & 0x1FFu) * 8192u;
  color_mask_ = static_cast<std::uint32_t>(frame >> 32) |
                (format == 1u ? 0xFF000000u : 0u);
  color_memory_ = memory && memory->size() == 4u * 1024u * 1024u &&
                  color_width_ != 0u && format <= 1u ? memory : nullptr;
}

std::uint32_t Gs::target_read(std::uint32_t address) const {
  std::uint32_t value = 0;
  for (unsigned b = 0; b < 4; ++b)
    value |= std::uint32_t{(*color_memory_)[(address + b) & 0x3FFFFFu]} << (8u * b);
  return value;
}

void Gs::target_write(int x, int y, std::uint32_t color, std::uint32_t mask) {
  if (static_cast<unsigned>(x * 4) >= color_width_) return;
  for (unsigned dy = 0; dy < 4; ++dy)
    for (unsigned dx = 0; dx < 4; ++dx) {
      const auto address = color_base_ +
          ((y * 4u + dy) * color_width_ + x * 4u + dx) * 4u;
      const auto value = (color & ~mask) | (target_read(address) & mask);
      for (unsigned b = 0; b < 4; ++b)
        (*color_memory_)[(address + b) & 0x3FFFFFu] = value >> (b * 8u);
    }
}

void Gs::clear(std::uint32_t color, std::uint32_t depth) {
  std::fill(color_.begin(), color_.end(), color);
  std::fill(depth_.begin(), depth_.end(), depth);
  if (color_memory_)
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) target_write(x, y, color, 0u);
}

void Gs::write(int x, int y, std::uint32_t z, std::uint32_t color) {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return;
  if (x < scissor_left_ || x > scissor_right_ ||
      y < scissor_top_ || y > scissor_bottom_) return;
  const auto index = static_cast<std::size_t>(y * kWidth + x);
  bool write_color = true, write_depth = depth_write_, preserve_alpha = false;
  if ((alpha_test_ & 1u) != 0u) {
    const unsigned alpha = color >> 24;
    const unsigned reference = (alpha_test_ >> 4) & 0xFFu;
    const bool results[] = {false, true, alpha < reference, alpha <= reference,
                            alpha == reference, alpha >= reference,
                            alpha > reference, alpha != reference};
    if (!results[(alpha_test_ >> 1) & 7u]) {
      switch ((alpha_test_ >> 12) & 3u) {
      case 0: return; // KEEP: neither buffer is updated.
      case 1: write_depth = false; break; // FB_ONLY
      case 2: write_color = false; break; // ZB_ONLY
      case 3: write_depth = false; preserve_alpha = true; break; // RGB_ONLY
      }
    }
  }
  const bool pass = depth_test_ == DepthTest::Always ||
      (depth_test_ == DepthTest::GreaterEqual && z >= depth_[index]) ||
      (depth_test_ == DepthTest::Greater && z > depth_[index]) ||
      (depth_test_ == DepthTest::LessEqual && z <= depth_[index]);
  if (pass) {
    if (write_depth) depth_[index] = z;
    if (!write_color) return;
    if (blend_enabled_ && (!blend_pabe_ || (color & 0x80000000u) != 0u)) {
      const auto destination = pixel(x, y);
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
    if (preserve_alpha) color = (color & 0x00FFFFFFu) | (pixel(x, y) & 0xFF000000u);
    if (color_memory_) {
      target_write(x, y, color, color_mask_ | (preserve_alpha ? 0xFF000000u : 0u));
      color = pixel(x, y);
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

void Gs::triangle(GsVertex a, GsVertex b, GsVertex c,
                  const TextureSampler& sample, unsigned st_width, unsigned st_height) {
  std::int64_t area = edge(a, b, c.x, c.y);
  // A collapsed triangle has no coverage; it is not a line primitive.
  if (area == 0) return;
  if (area < 0) { std::swap(b, c); area = -area; }
  const auto fp = [](std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return static_cast<double>(value);
  };
  const double sa = fp(static_cast<std::uint32_t>(a.st)),
               sb = fp(static_cast<std::uint32_t>(b.st)),
               sc = fp(static_cast<std::uint32_t>(c.st));
  const double ta = fp(static_cast<std::uint32_t>(a.st >> 32)),
               tb = fp(static_cast<std::uint32_t>(b.st >> 32)),
               tc = fp(static_cast<std::uint32_t>(c.st >> 32));
  const double qa = fp(a.q), qb = fp(b.q), qc = fp(c.q);
  // With positive edge() area and screen Y increasing downwards, include
  // left/downward and top/leftward edges; exclude their opposite partners.
  const auto inclusive = [](const GsVertex& from, const GsVertex& to) {
    return to.y > from.y || (to.y == from.y && to.x < from.x);
  };
  const bool include_a = inclusive(b, c), include_b = inclusive(c, a),
             include_c = inclusive(a, b);
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
      if ((wa == 0 && !include_a) || (wb == 0 && !include_b) ||
          (wc == 0 && !include_c)) continue;
      const auto z = static_cast<std::uint32_t>((
          static_cast<std::uint64_t>(a.z) * wa +
          static_cast<std::uint64_t>(b.z) * wb +
          static_cast<std::uint64_t>(c.z) * wc) / static_cast<std::uint64_t>(area));
      std::uint32_t color = 0;
      for (unsigned shift = 0; shift < 32; shift += 8)
        color |= mix_channel(a.color, b.color, c.color, wa, wb, wc, area, shift);
      if (sample) {
        // FST UV is unsigned 10.4 fixed point. Interpolate before removing
        // the fractional bits; vertex winding swaps must also swap attributes.
        const auto coordinate = [&](unsigned shift) {
          const auto sum = ((a.uv >> shift) & 0x3FFFu) * wa +
                           ((b.uv >> shift) & 0x3FFFu) * wb +
                           ((c.uv >> shift) & 0x3FFFu) * wc;
          return static_cast<unsigned>(sum / (area * 16));
        };
        if (st_width != 0u && st_height != 0u) {
          // S, T and Q are independently affine in screen space. Divide AFTER
          // interpolation; area cancels between numerator and denominator.
          const double q = qa * wa + qb * wb + qc * wc;
          if (q == 0.0 || !std::isfinite(q)) continue;
          const double u = (sa * wa + sb * wb + sc * wc) * st_width / q;
          const double v = (ta * wa + tb * wb + tc * wc) * st_height / q;
          // Safety boundary for the current unsigned logical-memory sampler.
          // Negative/nonfinite coordinates and GS clamp modes need a separate
          // hardware-validated implementation; never cast them with C++ UB.
          const double max = std::numeric_limits<unsigned>::max();
          if (!std::isfinite(u) || !std::isfinite(v) ||
              u < 0.0 || v < 0.0 || u > max || v > max) continue;
          color = sample(static_cast<unsigned>(u), static_cast<unsigned>(v), color);
        } else {
          color = sample(coordinate(0), coordinate(16), color);
        }
      }
      write(x, y, z, color);
    }
  }
}

std::uint32_t Gs::pixel(int x, int y) const {
  if (x < 0 || y < 0 || x >= kWidth || y >= kHeight) return 0;
  if (color_memory_) {
    if (static_cast<unsigned>(x * 4) >= color_width_) return 0;
    return target_read(color_base_ + (y * 4u * color_width_ + x * 4u) * 4u);
  }
  return color_[static_cast<std::size_t>(y * kWidth + x)];
}

const std::uint32_t* Gs::pixels() const {
  if (color_memory_)
    for (int y = 0; y < kHeight; ++y)
      for (int x = 0; x < kWidth; ++x) color_[y * kWidth + x] = pixel(x, y);
  return color_.data();
}

} // namespace ps2vita
