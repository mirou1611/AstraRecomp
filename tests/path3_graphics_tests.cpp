#include "ps2vita/emulator.hpp"
#include "ps2vita/framebuffer_dump.hpp"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>

// Self-contained logical-GS integration oracle, not a hardware golden dump.
// Execute guest EE stores to kick PATH3 DMA; never submit directly to Gif.
int main(int argc, char** argv) {
  const bool perspective = argc > 1 && std::strcmp(argv[1], "--stq") == 0;
  const bool rgb_decal = argc > 1 && std::strcmp(argv[1], "--rgb-decal") == 0;
  const bool rgb_modulate = argc > 1 && std::strcmp(argv[1], "--rgb-modulate") == 0;
  const bool highlight1 = argc > 1 && std::strcmp(argv[1], "--highlight") == 0;
  const bool highlight2 = argc > 1 && std::strcmp(argv[1], "--highlight2") == 0;
  const bool region_repeat = argc > 1 && std::strcmp(argv[1], "--region-repeat") == 0;
  // Diagnostic until shared framebuffer storage is implemented. Deliberately
  // returns failure on unsupported feedback; never mark it WILL_FAIL in CTest.
  const bool feedback = argc > 1 && std::strcmp(argv[1], "--feedback") == 0;
  const bool highlight = highlight1 || highlight2;
  const bool rgb = rgb_decal || rgb_modulate;
  if (perspective || rgb || highlight || region_repeat || feedback) { --argc; ++argv; }
  const unsigned expected_alpha = highlight1 ? 0x60u : highlight2 ? 0x20u :
                                  rgb ? 0x40u : 0xFFu;
  ps2vita::Emulator emulator;
  auto& memory = emulator.memory();
  std::vector<std::uint64_t> packet{
      0x1000000000008004ull, 0xEull,
      0x0001000100000000ull, 0x50u, 0u, 0x51u,
      0x0000000200000002ull, 0x52u, 0u, 0x53u,
      0x0800000000008001ull, 0u,
      0xFF00FF00FF0000FFull, 0xFFFFFFFFFFFF0000ull};
  if (highlight) {
    packet[12] = 0x2000FF00200000FFull;
    packet[13] = 0x20FFFFFF20FF0000ull;
  }
  const auto ad = [&](std::uint64_t value, std::uint64_t reg) {
    packet.insert(packet.end(), {0x1000000000008001ull, 0xEull, value, reg});
  };
  ad(1u, 0x1Au); // PRMODECONT: use PRIM attributes.
  // FRAME base 32 * 8192 = 0x40000, separate from texture at 0x100.
  ad(32u | (10ull << 16), 0x4Cu); // 640-wide PSMCT32 framebuffer.
  ad(128u | (1ull << 32), 0x4Eu); // Mask depth writes.
  ad(1u, 0x46u); // COLCLAMP
  ad(0u, 0x14u); // TEX1: nearest filtering, no mip selection.
  ad(region_repeat ? 3ull | (1ull << 14) : 0ull, 0x08u); // U=(U&0)|1, V repeat.
  ad(0u, 0x3Fu); // TEXFLUSH after the upload.
  ad(1ull | (1ull << 14) | (1ull << 26) | (1ull << 30) |
      (rgb ? 0ull : (1ull << 34)) |
      ((highlight1 ? 2ull : highlight2 ? 3ull : rgb_modulate ? 0ull : 1ull) << 35),
      6u); // 2x2 PSMCT32, selected TCC/TFX.
  ad(0u, 0x18u); // XYOFFSET
  ad(0x07FF000007FF0000ull, 0x40u); // SCISSOR
  ad(0u, 0x47u); // No alpha/depth test
  ad(6u, 0u); // Untextured, unblended sprite clears the full 640x448 target.
  ad(0u, 1u);
  ad(0u, 5u);
  const auto clear_kick_address_word = packet.size() + 3u;
  ad(10240u | (7168ull << 16), 5u);
  // RGB must retain vertex alpha=0x40, not use/modulate texture alpha=0xFF.
  // Make the alpha result observable in reference RGB captures too.
  if (rgb) ad(1u | (4u << 1) | (0x40u << 4), 0x47u); // EQUAL, KEEP
  if (highlight) ad(1u | (4u << 1) | (expected_alpha << 4), 0x47u);
  if (feedback) {
    // A=0x80000, B=0x40000. No IMAGE upload supplies A: it must come from draws.
    const auto solid = [&](std::uint32_t color) {
      ad(64u | (1ull << 16), 0x4Cu); // A, 64-wide PSMCT32
      ad(0u, 0x47u);
      ad(6u, 0u); ad(color, 1u);
      ad(0u, 5u); ad(128u | (128ull << 16), 5u); // 8x8 native
    };
    const auto copy = [&](unsigned x) {
      ad(32u | (10ull << 16), 0x4Cu); // B, 640-wide PSMCT32
      ad(0u, 0x3Fu); // TEXFLUSH
      ad(0x800ull | (1ull << 14) | (1ull << 20) | (3ull << 26) |
          (3ull << 30) | (1ull << 34) | (1ull << 35), 6u);
      ad(0x40u, 0x3Bu); // TEXA: TA0=64, AEM=0 (PSMCT24 alpha)
      ad(1u | (4u << 1) | (0x40u << 4), 0x47u); // EQUAL 64, KEEP
      ad(0x116u, 0u); ad(0x80808080u, 1u);
      ad(0u, 3u); ad(x * 16u, 5u);
      ad(128u | (128ull << 16), 3u);
      ad((x + 8u) * 16u | (128ull << 16), 5u);
    };
    solid(0x800000FFu); copy(0u); // red A -> left B
    solid(0x80FF0000u); copy(8u); // blue A -> right B; left B must stay red
  } else if (perspective) {
    ad(0x13u, 0u);
    ad(0x3F80000080808080ull, 1u); // Q=1
    ad(0u, 2u); ad(0u, 5u);
    ad(0x4000000080808080ull, 1u); // Q=2
    ad(0x40000000u, 2u); ad(2048u, 5u); // S=2, T=0
    ad(0x3F80000080808080ull, 1u); // Q=1
    ad(0x3F80000000000000ull, 2u); ad(2048ull << 16, 5u); // S=0, T=1
  } else {
    ad(0x113u, 0u);
    ad(rgb || highlight ? 0x40808080u : 0x80808080u, 1u);
    ad(0u, 3u); ad(0u, 5u);
    ad(region_repeat ? 64u : 32u, 3u); ad(2048u, 5u);
    ad((region_repeat ? 64ull : 32ull) << 16, 3u); ad(2048ull << 16, 5u);
  }
  // An off-scissor point changes primitive class and submits the pending
  // triangle batch in the reference renderer without needing display scanout.
  ad(0u, 0u);
  ad(0xFFFFFFFFu, 5u);
  constexpr std::uint32_t entry = 0x100000u, packet_address = 0x101000u;
  const std::uint32_t program[]{
      0x3C081001u, // lui t0, 0x1001 (SW sign-extends the Axxx offset)
      0x8D09E000u, // lw t1, D_CTRL(t0)
      0x35290001u, // ori t1, t1, DMAE
      0xAD09E000u, // sw t1, D_CTRL(t0), preserve other bits
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
      0x0000000Du, // break: host test completion, not a hardware exit service
      0u          // reserved delay slot for the persistent-loop variant
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
  // Poison after loading: only the guest clear may establish the background.
  emulator.gs().clear(0xA55AA55Au);
  const auto reason = emulator.run_slice(10000u); // Hard bound on guest poll.
  bool ok = loaded.ok && loaded.entry == entry && loaded.segments == 1u &&
      reason == ps2vita::StopReason::Break &&
      emulator.gif().triangles_emitted() == (feedback ? 0u : 1u) &&
      emulator.gif().sprites_emitted() == (feedback ? 5u : 1u) &&
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
      const bool green = perspective ? 3 * x >= 32 : x >= 16;
      const bool blue = perspective ? 2 * y >= 32 + x : y >= 16;
      std::uint32_t expected = x + y < 32 ?
          ((expected_alpha << 24) | (highlight ? 0x00404040u : 0u) |
           (green ? 0x0000FF00u : blue ? 0x00FF0000u : 0x000000FFu)) : 0u;
      if (region_repeat && x + y < 32)
        expected = ((y / 8) % 2) ? 0xFFFFFFFFu : 0xFF00FF00u;
      if (feedback)
        expected = y < 2 && x < 4 ? (x < 2 ? 0x400000FFu : 0x40FF0000u) : 0u;
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
  // Independently execute the exported persistent variant too. Its terminal
  // loop must be reached after DMA, not merely consume the poll's host budget.
  const auto done = entry + sizeof(program) - 8u;
  put(0x100u + sizeof(program) - 8u, 0x08000000u | (done >> 2), 4);
  std::vector<std::uint32_t> expected_pixels(emulator.gs().pixels(),
      emulator.gs().pixels() + ps2vita::Gs::kWidth * ps2vita::Gs::kHeight);
  const auto loop_loaded = emulator.load_elf(elf.data(), elf.size());
  emulator.gs().clear(0x5AA55AA5u);
  const auto loop_reason = emulator.run_slice(10000u);
  const bool at_done = emulator.cpu().state().pc == done ||
                      emulator.cpu().state().pc == done + 4u;
  ok = ok && loop_loaded.ok && loop_reason == ps2vita::StopReason::StepLimit &&
      at_done && emulator.gif().triangles_emitted() == (feedback ? 0u : 1u) &&
      emulator.vif1().packets_submitted() == 0u &&
      (memory.read32(0x1000A000u) & 0x100u) == 0u;
  for (std::size_t i = 0; i < expected_pixels.size(); ++i)
    ok = (emulator.gs().pixels()[i] == expected_pixels[i]) && ok;
  if (argc >= 4) {
    std::ofstream output(argv[3], std::ios::binary);
    output.write(reinterpret_cast<const char*>(elf.data()), elf.size());
    ok = static_cast<bool>(output) && ok;
  }
  // Negative control: XYZ3 advances the sprite without drawing it. The far
  // background must remain poisoned, demonstrating this gate detects a missing
  // guest clear instead of accidentally relying on reset-time black pixels.
  put(0x100u + packet_address - entry + clear_kick_address_word * 8u, 0xDu, 8);
  const auto negative_loaded = emulator.load_elf(elf.data(), elf.size());
  emulator.gs().clear(0x12345678u);
  const auto negative_reason = emulator.run_slice(10000u);
  ok = ok && negative_loaded.ok && negative_reason == ps2vita::StopReason::StepLimit &&
      emulator.gif().sprites_emitted() == (feedback ? 4u : 0u) &&
      emulator.gs().pixel(ps2vita::Gs::kWidth - 1, ps2vita::Gs::kHeight - 1) ==
          0x12345678u;
  std::cout << (feedback ? "PATH3 framebuffer-feedback: " : "PATH3 EE-store/DMA/textured-triangle: ") << (ok ? "PASS" : "FAIL")
            << " pixel_mismatches=" << mismatches << '\n';
  return ok ? 0 : 1;
}
