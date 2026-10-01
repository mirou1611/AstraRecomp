#include "ps2vita/vu.hpp"
#include <array>
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
      const bool trace_enabled = ((numerator ^ denominator) & 1u) != 0u;
      vu.enable_store_trace(trace_enabled);
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
      check(vu.div_records().size() == (trace_enabled ? 1u : 0u),
            "DIV diagnostics are opt-in", numerator, denominator);
      if (trace_enabled) {
        const auto& record = vu.div_records().front();
        check(record.numerator == numerator && record.denominator == denominator &&
              record.result == expected && record.pc == 0u && record.pair == 0u &&
              record.cycle == 0u && record.ready_cycle == 7u,
              "DIV diagnostics preserve actual operands and pending result", numerator, denominator);
      }
    }
  }
  // These finite goldens were observed independently in BOTH PCSX2 v2.8.2
  // microVU and interpreter, with VU1Roundmode=3, using the owned VIF fixture
  // from tools/make_vu1_math_fixture.py. They are not native-float expectations.
  constexpr std::array<std::array<std::uint32_t, 3>, 10> finite{{
      {{0x3F800000u, 0x40200000u, 0x3ECCCCCCu}},
      {{0x3F800000u, 0xC0200000u, 0xBECCCCCCu}},
      {{0xBF800000u, 0x40200000u, 0xBECCCCCCu}},
      {{0xBF800000u, 0xC0200000u, 0x3ECCCCCCu}},
      {{0x3F800000u, 0x40400000u, 0x3EAAAAAAu}},
      {{0x3F800000u, 0xC0400000u, 0xBEAAAAAAu}},
      {{0xBF800000u, 0x40400000u, 0xBEAAAAAAu}},
      {{0xBF800000u, 0xC0400000u, 0x3EAAAAAAu}},
      {{0x3F800000u, 0x40000000u, 0x3F000000u}},
      {{0xBF800000u, 0x40000000u, 0xBF000000u}},
  }};
  for (const auto& row : finite) {
    const auto numerator = row[0], denominator = row[1], expected = row[2];
    ps2vita::Vu1 vu(memory);
    vu.enable_store_trace(true);
    vu.state().vf[1][0] = numerator;
    vu.state().vf[2][0] = denominator;
    vu.state().q = 0x3F800000u;
    vu.start(0u);
    vu.run(1u);
    check(vu.state().q == 0x3F800000u, "finite Q stays pending", numerator, denominator);
    check(vu.div_records().size() == 1u && vu.div_records()[0].result == expected,
          "finite DIV records chopped pending result", numerator, denominator);
    vu.run(2u);
    check(vu.state().q == expected && vu.state().vf[3][3] == expected,
          "finite DIV Q and subsequent consumer match both reference modes", numerator, denominator);
    check(vu.cycles_executed() == 9u && vu.q_stall_cycles() == 6u,
          "finite DIV timing is unchanged", numerator, denominator);
  }
  std::printf("VU1 DIV checks=%u failures=%u\n", checks, failures);
  return failures ? 1 : 0;
}
