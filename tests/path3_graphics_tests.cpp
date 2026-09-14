#include "ps2vita/emulator.hpp"
#include "ps2vita/framebuffer_dump.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

// Self-contained logical-GS integration oracle, not a hardware golden dump.
// Execute guest EE stores to kick PATH3 DMA; never submit directly to Gif.
int main(int argc, char** argv) {
  ps2vita::Emulator emulator;
  auto& memory = emulator.memory();
  emulator.gs().clear(0u);
  std::vector<std::uint64_t> packet{
      0x1000000000008004ull, 0xEull,
      0x0001000100000000ull, 0x50u, 0u, 0x51u,
      0x0000000200000002ull, 0x52u, 0u, 0x53u,
      0x0800000000008001ull, 0u,
      0xFF00FF00FF0000FFull, 0xFFFFFFFFFFFF0000ull};
  const auto ad = [&](std::uint64_t value, std::uint64_t reg) {
    packet.insert(packet.end(), {0x1000000000008001ull, 0xEull, value, reg});
  };
  ad(1ull | (1ull << 14) | (1ull << 26) | (1ull << 30) |
      (1ull << 34) | (1ull << 35), 6u); // 2x2 PSMCT32, DECAL RGBA
  ad(0u, 0x18u); // XYOFFSET
  ad(0x07FF000007FF0000ull, 0x40u); // SCISSOR
  ad(0u, 0x47u); // No alpha/depth test
  ad(0x113u, 0u); // triangle + texture + fixed UV
  ad(0x80808080u, 1u);
  ad(0u, 3u); ad(0u, 5u);
  ad(32u, 3u); ad(2048u, 5u);
  ad(32ull << 16, 3u); ad(2048ull << 16, 5u);
  constexpr std::uint32_t entry = 0x100000u, packet_address = 0x101000u;
  const std::uint32_t program[]{
      0x3C081001u, // lui t0, 0x1001 (SW sign-extends the Axxx offset)
      0x3C090010u, // lui t1, 0x0010
      0x35291000u, // ori t1, t1, packet address low half
      0xAD09A010u, // sw t1, D2_MADR(t0)
      0x34090000u | static_cast<std::uint32_t>(packet.size() / 2u),
      0xAD09A020u, // sw t1, D2_QWC(t0)
      0x34090101u, // direction=from memory, start
      0xAD09A000u, // sw t1, D2_CHCR(t0)
      0x8D09A000u, // poll: lw t1, D2_CHCR(t0)
      0x31290100u, // andi t1, t1, STR
      0x1520FFFDu, // bne t1, zero, poll
      0u,         // delay slot
      0x0000000Du // break: host test completion, not a hardware exit service
  };
  // ELF32 little-endian MIPS executable, one load segment containing code and
  // aligned packet data. Emit integers explicitly, independent of host endian.
  const auto segment_size = packet_address - entry + packet.size() * 8u;
  std::vector<std::uint8_t> elf(0x100u + segment_size);
  const auto put = [&](std::size_t offset, std::uint64_t value, unsigned bytes) {
    for (unsigned byte = 0; byte < bytes; ++byte)
      elf[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  put(0, 0x464C457Fu, 4); put(4, 0x010101u, 3);
  put(16, 2, 2); put(18, 8, 2); put(20, 1, 4);
  put(24, entry, 4); put(28, 52, 4);
  put(40, 52, 2); put(42, 32, 2); put(44, 1, 2);
  put(52, 1, 4); put(56, 0x100, 4);
  put(60, entry, 4); put(64, entry, 4);
  put(68, segment_size, 4); put(72, segment_size, 4);
  put(76, 5, 4); put(80, 16, 4);
  for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
    put(0x100u + i * 4u, program[i], 4);
  for (std::size_t i = 0; i < packet.size(); ++i)
    put(0x100u + packet_address - entry + i * 8u, packet[i], 8);
  const auto loaded = emulator.load_elf(elf.data(), elf.size());
  const auto reason = emulator.run_slice(10000u); // Hard bound on guest poll.
  bool ok = loaded.ok && loaded.entry == entry && loaded.segments == 1u &&
      reason == ps2vita::StopReason::Break &&
      emulator.gif().triangles_emitted() == 1u &&
      emulator.gif().packets_rejected() == 0u &&
      emulator.gif().pending_bytes() == 0u &&
      emulator.vif1().packets_submitted() == 0u &&
      emulator.vif1().vu1().pairs_executed() == 0u &&
      memory.read32(0x1000A020u) == 0u &&
      (memory.read32(0x1000A000u) & 0x100u) == 0u;
  unsigned mismatches = 0;
  for (int y = 0; y < ps2vita::Gs::kHeight; ++y)
    for (int x = 0; x < ps2vita::Gs::kWidth; ++x) {
      // Independently specified coverage and nearest texels: no rasterizer
      // helpers or captured output are used to generate the expectation.
      const std::uint32_t expected = x + y < 32 ?
          (x >= 16 ? 0xFF00FF00u : y >= 16 ? 0xFFFF0000u : 0xFF0000FFu) : 0u;
      if (emulator.gs().pixel(x, y) != expected) ++mismatches;
    }
  ok = ok && mismatches == 0u;
  if (argc >= 2) {
    std::ofstream output(argv[1], std::ios::binary);
    ok = ps2vita::write_framebuffer_ppm(output, emulator.gs()) && ok;
  }
  if (argc >= 3) {
    std::ofstream output(argv[2], std::ios::binary);
    output.write(reinterpret_cast<const char*>(elf.data()), elf.size());
    ok = static_cast<bool>(output) && ok;
  }
  std::cout << "PATH3 EE-store/DMA/textured-triangle: " << (ok ? "PASS" : "FAIL")
            << " pixel_mismatches=" << mismatches << '\n';
  return ok ? 0 : 1;
}
