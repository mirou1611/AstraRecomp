#pragma once

#include "ps2vita/gs.hpp"
#include <ostream>

namespace ps2vita {

// Diagnostic RGB snapshot: GS host pixels are AABBGGRR. Alpha is deliberately
// omitted so a nonzero alpha byte cannot masquerade as visible RGB output.
inline bool write_framebuffer_ppm(std::ostream& output, const Gs& gs) {
  output << "P6\n" << Gs::kWidth << ' ' << Gs::kHeight << "\n255\n";
  for (int y = 0; y < Gs::kHeight; ++y) {
    for (int x = 0; x < Gs::kWidth; ++x) {
      const auto pixel = gs.pixel(x, y);
      const char rgb[]{static_cast<char>(pixel & 0xFFu),
                       static_cast<char>((pixel >> 8) & 0xFFu),
                       static_cast<char>((pixel >> 16) & 0xFFu)};
      output.write(rgb, sizeof(rgb));
    }
  }
  return static_cast<bool>(output);
}

} // namespace ps2vita
