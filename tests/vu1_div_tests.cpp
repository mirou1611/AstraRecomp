#include "ps2vita/vu.hpp"
#include <cstdio>

// Architectural Q outcomes for zero/denormal operands, independently checked
// against PCSX2 v2.8.2 VUops.cpp (_vuDIV and vuDouble). Status I/D flags are
// outside the current Vu1State model; this test makes no claim about them.
int main() {
  ps2vita::Memory memory;
  unsigned checks = 0, failures = 0;
  const auto check = [&](bool condition, const char* name,
                         std::uint32_t numerator, std::uint32_t denominator) {
    ++checks;
    if (!condition) {
      ++failures;
      std::fprintf(stderr, "%s numerator=%08X denominator=%08X\n",
                   name, numerator, denominator);
    }
  };
  for (auto numerator : {0u, 0x80000000u, 0x3F800000u, 0xBF800000u,
                         1u, 0x80000001u}) {
    for (auto denominator : {0u, 0x80000000u, 1u, 0x80000001u,
                             0x3F800000u, 0xBF800000u}) {
      ps2vita::Vu1 vu(memory);
      vu.state().vf[1][0] = numerator;
      vu.state().vf[2][0] = denominator;
      vu.state().q = 0x3F800000u;
      memory.vu1_store_micro_word(0u, 0x800003BCu | (1u << 11) | (2u << 16));
      memory.vu1_store_micro_word(4u, 0x000002FFu);
      memory.vu1_store_micro_word(8u, 0x800003BFu); // WAITQ
      memory.vu1_store_micro_word(12u, 0x000002FFu);
      memory.vu1_store_micro_word(16u, 0x8000033Cu);
      memory.vu1_store_micro_word(20u, 0x01E00000u | (3u << 6) | 0x1Cu); // MULq vf3,vf0
      vu.start(0u);
      vu.run(1u);
      check(vu.state().q == 0x3F800000u, "Q stays pending", numerator, denominator);
      vu.run(1u);
      const auto magnitude = (denominator & 0x7F800000u) == 0u ? 0x7F7FFFFFu :
          (numerator & 0x7F800000u) == 0u ? 0u : 0x3F800000u;
      const auto expected = ((numerator ^ denominator) & 0x80000000u) | magnitude;
      check(vu.state().q == expected, "DIV boundary Q with XOR sign", numerator, denominator);
      check(vu.cycles_executed() == 8u && vu.q_stall_cycles() == 6u,
            "WAITQ preserves seven-cycle DIV latency", numerator, denominator);
      vu.run(1u);
      check(vu.state().vf[3][3] == expected, "MULq consumes saturated Q", numerator, denominator);
      check(!vu.first_unsupported_lower() && !vu.first_unsupported_upper(),
            "supported instructions", numerator, denominator);
    }
  }
  std::printf("VU1 zero-DIV checks=%u failures=%u\n", checks, failures);
  return failures ? 1 : 0;
}
