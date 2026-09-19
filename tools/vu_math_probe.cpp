#include "ps2vita/vu.hpp"
#include <cstdio>
#include <cstring>

namespace {
bool hex32(const char* text, std::uint32_t& value) {
  if (std::strlen(text) != 8u) return false;
  value = 0;
  for (unsigned i = 0; i < 8u; ++i) {
    const char c = text[i];
    const unsigned digit = c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 :
        c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16u;
    if (digit == 16u) return false;
    value = (value << 4) | digit;
  }
  return true;
}
}

// Raw-bit arithmetic probe, not a hardware oracle or pipeline timing test.
int main(int argc, char** argv) {
  std::uint32_t lhs = 0, rhs = 0, acc = 0, expected = 0;
  if ((argc != 5 && argc != 6) ||
      (std::strcmp(argv[1], "mul") && std::strcmp(argv[1], "madd")) ||
      !hex32(argv[2], lhs) || !hex32(argv[3], rhs) || !hex32(argv[4], acc) ||
      (argc == 6 && !hex32(argv[5], expected))) {
    std::fprintf(stderr, "usage: ps2vu_math_probe mul|madd LHS RHS ACC [EXPECTED]\n"
        "Each value is exactly eight hexadecimal digits (raw float bits).\n");
    return 2;
  }
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  auto& state = vu.state();
  state.vf[1].fill(lhs);
  state.vf[2].fill(rhs);
  state.acc.fill(acc);
  const std::uint32_t upper = 0x01E00000u | (2u << 16) | (1u << 11) |
      (3u << 6) | (std::strcmp(argv[1], "mul") == 0 ? 0x2Au : 0x08u);
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8000033Cu); // lower NOP
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, upper);
  vu.start(0u);
  vu.run(1u);
  std::printf("operation=%s lower=8000033C upper=%08X lhs=%08X rhs=%08X acc=%08X\n",
              argv[1], upper, lhs, rhs, acc);
  std::printf("vf3=%08X,%08X,%08X,%08X mac=%04X pc=%04X pairs=%llu cycles=%llu\n",
      state.vf[3][0], state.vf[3][1], state.vf[3][2], state.vf[3][3],
      state.mac, state.pc, static_cast<unsigned long long>(vu.pairs_executed()),
      static_cast<unsigned long long>(vu.cycles_executed()));
  if (vu.pairs_executed() != 1u || state.pc != 8u ||
      vu.first_unsupported_upper() || vu.first_unsupported_lower()) return 1;
  for (unsigned lane = 0; lane < 4u; ++lane) {
    if (state.acc[lane] != acc || state.vf[1][lane] != lhs ||
        state.vf[2][lane] != rhs) return 1;
    if (argc == 6 && state.vf[3][lane] != expected) {
      std::fprintf(stderr, "mismatch lane=%u expected=%08X actual=%08X\n",
                   lane, expected, state.vf[3][lane]);
      return 3;
    }
  }
  return 0;
}
