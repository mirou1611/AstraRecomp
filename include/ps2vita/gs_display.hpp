#pragma once

#include <cstdint>

namespace ps2vita {

struct GsDisplayFramebuffer {
  std::uint32_t fbp = 0, fbw = 0, psm = 0, dbx = 0, dby = 0;

  static constexpr GsDisplayFramebuffer decode(std::uint64_t value) {
    return {static_cast<std::uint32_t>(value & 0x1FFu),
            static_cast<std::uint32_t>((value >> 9u) & 0x3Fu),
            static_cast<std::uint32_t>((value >> 15u) & 0x1Fu),
            static_cast<std::uint32_t>((value >> 32u) & 0x7FFu),
            static_cast<std::uint32_t>((value >> 43u) & 0x7FFu)};
  }

  // DISPFB FBP and drawing FRAME FBP both select 32 256-byte blocks.
  // PCSX2 v2.8.2 GSRegDISPFB::Block() and GIFRegFRAME::Block() agree.
  constexpr std::uint32_t base_bytes() const { return fbp * 8192u; }
  constexpr std::uint32_t width_pixels() const { return fbw * 64u; }
};

} // namespace ps2vita
