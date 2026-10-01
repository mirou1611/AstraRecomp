#include "ps2vita/vu.hpp"

#include <array>
#include <cstdio>

// First four raw products are recorded PS2 VU0 MULi outcomes, transposed here
// to VU1 MUL/MULq, not hardware validation of these instruction forms:
// https://github.com/unknownbrackets/ps2autotests/blob/
// 97469ffbed8631277b94e28d01dabd702aa97ef3/tests/vu/games/triace.expected
// All six inexact rows also match both stock PCSX2 v2.8.2 VU1 execution modes
// on the owned tools/make_vu1_math_fixture.py packet. They do NOT distinguish
// the known PS2 partial-product correction from ordinary exact-product chop.
int main() {
  constexpr std::array<std::array<std::uint32_t, 3>, 8> rows{{
      {{0xB063B75Bu, 0x42FECCCDu, 0xB3E2A618u}},
      {{0x8701CE82u, 0x43D80D3Eu, 0x8B5B19E9u}},
      {{0x46FCC888u, 0x43A0DA10u, 0x4B1ED4A7u}},
      {{0x793CC535u, 0x43546E14u, 0x7D1CA47Bu}},
      {{0x3C23D70Au, 0x3ECCCCCCu, 0x3B83126Du}},
      {{0x3C23D70Au, 0x3ECCCCCDu, 0x3B83126Eu}},
      {{0x40000000u, 0x40400000u, 0x40C00000u}},
      {{0xC0000000u, 0x40400000u, 0xC0C00000u}},
  }};
  ps2vita::Memory memory;
  unsigned checks = 0, failures = 0;
  for (unsigned row = 0; row < rows.size(); ++row) {
    for (const unsigned opcode : {0x2Au, 0x1Cu}) {
      for (const unsigned mask : {15u, 10u, 5u, 0u}) {
        for (const unsigned destination : {0u, 1u, 2u, 3u}) {
          const auto check = [&](bool condition, const char* label) {
            ++checks;
            if (!condition) {
              ++failures;
              std::fprintf(stderr, "FAIL %s row=%u opcode=%02X mask=%X fd=%u\n",
                           label, row, opcode, mask, destination);
            }
          };
          ps2vita::Vu1 vu(memory);
          vu.state().vf[1].fill(rows[row][0]);
          vu.state().vf[2].fill(rows[row][1]);
          vu.state().vf[3].fill(0xDEADBEEFu);
          vu.state().q = rows[row][1];
          vu.state().mac = 0xFFFFu;
          const auto before = vu.state().vf[destination];
          const auto upper = (mask << 21) | (1u << 11) |
              (destination << 6) | opcode | (opcode == 0x2Au ? 2u << 16 : 0u);
          memory.vu1_store_micro_word(0u, 0x8000033Cu);
          memory.vu1_store_micro_word(4u, upper);
          vu.start(0u);
          vu.run(1u);
          for (unsigned lane = 0; lane < 4u; ++lane) {
            const auto expected = destination != 0u && (mask & (8u >> lane)) != 0u ?
                rows[row][2] : before[lane];
            check(vu.state().vf[destination][lane] == expected,
                  "finite product and masked/aliased destination");
          }
          check(vu.state().vf[0] == std::array<std::uint32_t, 4>{{0u, 0u, 0u, 0x3F800000u}},
                "VF0 remains immutable");
          check(vu.state().mac == ((rows[row][2] >> 31) ? mask << 4 : 0u),
                "MAC sign bits and inactive lanes, including discarded VF0 results");
          check(vu.cycles_executed() == 1u && vu.vf_stall_cycles() == 0u &&
                    vu.q_stall_cycles() == 0u && vu.state().q == rows[row][1],
                "rounding does not change timing or Q");
          check(!vu.first_unsupported_upper() && !vu.first_unsupported_lower(),
                "supported finite multiply forms");
        }
      }
    }
  }
  // Guard the normal-exponent edges and the existing underflow/overflow MAC
  // path. These independently specified model boundaries are not an expanded
  // hardware multiplier corpus; exponent-FF inputs remain out of scope.
  constexpr std::array<std::array<std::uint32_t, 4>, 8> boundaries{{
      {{0x00800000u, 0x3F800000u, 0x00800000u, 0u}},
      {{0x7F7FFFFFu, 0x3F800000u, 0x7F7FFFFFu, 0u}},
      // Exact product still has exponent 254; host nearest used to round it
      // to Inf and falsely report MAC overflow. Chopping retains max finite.
      {{0x7F7FFFFEu, 0x3F800001u, 0x7F7FFFFFu, 0u}},
      {{0x00800000u, 0x3F000000u, 0u, 0x0F0Fu}},
      {{0x80800000u, 0x3F000000u, 0x80000000u, 0x0FFFu}},
      {{0x7F7FFFFFu, 0x40000000u, 0x7F7FFFFFu, 0xF000u}},
      {{0xFF7FFFFFu, 0x40000000u, 0xFF7FFFFFu, 0xF0F0u}},
      {{0x80000000u, 0x3F800000u, 0x80000000u, 0x00FFu}},
  }};
  for (const auto& row : boundaries) {
    for (const unsigned opcode : {0x2Au, 0x1Cu}) {
      ps2vita::Vu1 vu(memory);
      vu.state().vf[1].fill(row[0]);
      vu.state().vf[2].fill(row[1]);
      vu.state().q = row[1];
      memory.vu1_store_micro_word(4u, (15u << 21) | (1u << 11) |
          (3u << 6) | opcode | (opcode == 0x2Au ? 2u << 16 : 0u));
      vu.start(0u);
      vu.run(1u);
      ++checks;
      if (vu.state().vf[3] != std::array<std::uint32_t, 4>{{row[2], row[2], row[2], row[2]}} ||
          vu.state().mac != row[3]) {
        ++failures;
        std::fprintf(stderr, "FAIL boundary lhs=%08X rhs=%08X opcode=%02X result=%08X mac=%04X\n",
                     row[0], row[1], opcode, vu.state().vf[3][0], vu.state().mac);
      }
    }
  }
  std::printf("VU1 finite MUL checks=%u failures=%u\n", checks, failures);
  return failures ? 1 : 0;
}
