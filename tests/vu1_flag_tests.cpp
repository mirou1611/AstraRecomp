#include "ps2vita/vu.hpp"

#include <array>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>

namespace {
using Vector = std::array<std::uint32_t, 4>;

constexpr std::uint32_t kLowerNop = 0x8000033Cu;
constexpr std::uint32_t kUpperNop = 0x000002FFu;
constexpr Vector kVf0{{0u, 0u, 0u, 0x3F800000u}};
constexpr Vector kResults{{0u, 0xBF800000u, 0x3F800000u, 0x80000000u}};
constexpr Vector kDestinationPoison{{0x41200000u, 0x41300000u, 0x41400000u, 0x41500000u}};
constexpr Vector kSignedValues{{0u, 0xBF800000u, 0x3F800000u, 0x80000000u}};
constexpr Vector kOnes{{0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u}};
constexpr Vector kZeros{{0u, 0u, 0u, 0u}};
constexpr Vector kNegativeZeros{{0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}};
constexpr Vector kAddend{{0u, 0u, 0u, 0x80000000u}};

unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const std::string& context) {
  ++checks;
  if (condition) return;
  ++failures;
  if (failures <= 32u) std::cerr << "FAIL: " << context << '\n';
}

void check_value(std::uint64_t actual, std::uint64_t expected,
                 const std::string& context) {
  ++checks;
  if (actual == expected) return;
  ++failures;
  if (failures <= 32u) {
    std::cerr << "FAIL: " << context << ": expected 0x" << std::hex
              << expected << ", got 0x" << actual << std::dec << '\n';
  }
}

std::uint32_t arithmetic(unsigned function, unsigned mask, unsigned destination) {
  return (mask << 21) | (2u << 16) | (1u << 11) |
      (destination << 6) | function;
}

std::uint32_t fmand(unsigned destination, unsigned source = 12u) {
  return (0x1Au << 25) | (destination << 16) | (source << 11);
}

std::uint32_t iaddiu(unsigned destination, unsigned immediate) {
  return (0x08u << 25) | (destination << 16) | (immediate & 0x7FFu) |
      ((immediate & 0x7800u) << 10);
}

void pair(ps2vita::Memory& memory, unsigned index, std::uint32_t lower,
          std::uint32_t upper = kUpperNop) {
  memory.vu1_store_micro_word(static_cast<std::uint16_t>(index * 8u), lower);
  memory.vu1_store_micro_word(static_cast<std::uint16_t>(index * 8u + 4u), upper);
}

struct MaskCase {
  unsigned mask;
  std::uint16_t mac;
  const char* name;
};

// Results are +0, -1, +1, -0. MAC has zero at x/w and sign at y/w.
// Seed every flag before issue: disabled lanes must clear all four flag kinds.
constexpr std::array<MaskCase, 4> kMasks{{
    {0xFu, 0x0059u, "xyzw"}, {0xAu, 0x0008u, "xz"},
    {0x5u, 0x0051u, "yw"}, {0x0u, 0x0000u, "none"}}};

struct ArithmeticCase {
  const char* name;
  unsigned function;
  Vector lhs;
  Vector rhs;
  Vector acc;
  std::uint32_t q;
};

// Every implemented VF-destination arithmetic opcode that updates MAC.
// Broadcast sources use identical components, while all component encodings
// are executed. Expected results and flags are constants, not emulator output.
constexpr std::array<ArithmeticCase, 16> kArithmeticCases{{
    {"ADD", 0x28u, {{0x3F800000u, 0xC0000000u, 0x3F800000u, 0x80000000u}},
        {{0xBF800000u, 0x3F800000u, 0u, 0x80000000u}}, kZeros, 0u},
    {"SUB", 0x2Cu, {{0x3F800000u, 0u, 0x40000000u, 0x80000000u}},
        {{0x3F800000u, 0x3F800000u, 0x3F800000u, 0u}}, kZeros, 0u},
    {"MUL", 0x2Au, kSignedValues, kOnes, kZeros, 0u},
    {"ADDx", 0x00u, kSignedValues, kNegativeZeros, kZeros, 0u},
    {"ADDy", 0x01u, kSignedValues, kNegativeZeros, kZeros, 0u},
    {"ADDz", 0x02u, kSignedValues, kNegativeZeros, kZeros, 0u},
    {"ADDw", 0x03u, kSignedValues, kNegativeZeros, kZeros, 0u},
    {"SUBx", 0x04u, kSignedValues, kZeros, kZeros, 0u},
    {"SUBy", 0x05u, kSignedValues, kZeros, kZeros, 0u},
    {"SUBz", 0x06u, kSignedValues, kZeros, kZeros, 0u},
    {"SUBw", 0x07u, kSignedValues, kZeros, kZeros, 0u},
    {"MADDx", 0x08u, kSignedValues, kOnes, kAddend, 0u},
    {"MADDy", 0x09u, kSignedValues, kOnes, kAddend, 0u},
    {"MADDz", 0x0Au, kSignedValues, kOnes, kAddend, 0u},
    {"MADDw", 0x0Bu, kSignedValues, kOnes, kAddend, 0u},
    {"MULq", 0x1Cu, kSignedValues, kZeros, kZeros, 0x3F800000u}}};

void test_arithmetic_destinations(ps2vita::Memory& memory) {
  ps2vita::Vu1 vu(memory);
  for (const auto& mask : kMasks) {
    for (const auto& operation : kArithmeticCases) {
      for (const unsigned destination : {0u, 3u}) {
        vu.reset();
        vu.state().vf[1] = operation.lhs;
        vu.state().vf[2] = operation.rhs;
        vu.state().vf[3] = kDestinationPoison;
        vu.state().acc = operation.acc;
        vu.state().q = operation.q;
        vu.state().mac = 0xFFFFu;
        pair(memory, 0u, kLowerNop,
             arithmetic(operation.function, mask.mask, destination));
        vu.start(0u);
        vu.run(1u);
        const std::string context = std::string(operation.name) + "." + mask.name +
            " VF" + std::to_string(destination);
        check_value(vu.state().mac, mask.mac, context + " MAC");
        check(vu.state().vf[0] == kVf0, context + " preserves VF0");
        check(vu.state().vf[1] == operation.lhs && vu.state().vf[2] == operation.rhs,
              context + " preserves sources");
        check(vu.state().acc == operation.acc, context + " preserves ACC");
        Vector expected = kDestinationPoison;
        if (destination != 0u) {
          for (unsigned lane = 0; lane < 4u; ++lane)
            if ((mask.mask & (8u >> lane)) != 0u) expected[lane] = kResults[lane];
        }
        check(vu.state().vf[3] == expected, context + " destination lanes");
        check_value(vu.pairs_executed(), 1u, context + " supported pair");
      }
    }
  }
}

void test_accumulator_controls(ps2vita::Memory& memory) {
  ps2vita::Vu1 vu(memory);
  for (const auto& mask : kMasks) {
    for (unsigned component = 0; component < 4u; ++component) {
      for (const unsigned selector : {2u, 6u}) {
        vu.reset();
        vu.state().vf[1] = kSignedValues;
        vu.state().vf[2] = kOnes;
        vu.state().acc = selector == 2u ? kAddend : kDestinationPoison;
        const Vector old_acc = vu.state().acc;
        vu.state().mac = 0xFFFFu;
        pair(memory, 0u, kLowerNop,
             arithmetic(0x3Cu + component, mask.mask, selector));
        vu.start(0u);
        vu.run(1u);
        const std::string context = std::string(selector == 2u ? "MADDA" : "MULA") +
            "xyzw"[component] + "." + mask.name;
        Vector expected = old_acc;
        for (unsigned lane = 0; lane < 4u; ++lane)
          if ((mask.mask & (8u >> lane)) != 0u) expected[lane] = kResults[lane];
        check_value(vu.state().mac, mask.mac, context + " MAC");
        check(vu.state().acc == expected, context + " ACC lanes");
        check(vu.state().vf[0] == kVf0, context + " preserves VF0");
      }
    }
  }
}

void test_max_preserves_mac(ps2vita::Memory& memory) {
  ps2vita::Vu1 vu(memory);
  for (const auto& mask : kMasks) {
    for (const unsigned function : {0x2Bu, 0x10u, 0x11u, 0x12u, 0x13u}) {
      vu.reset();
      vu.state().vf[1] = kSignedValues;
      vu.state().vf[2] = kOnes;
      vu.state().mac = 0xA5C3u;
      pair(memory, 0u, kLowerNop, arithmetic(function, mask.mask, 0u));
      vu.start(0u);
      vu.run(1u);
      const std::string context = "MAX function " + std::to_string(function) +
          "." + mask.name + " VF0";
      check_value(vu.state().mac, 0xA5C3u, context + " preserves MAC");
      check(vu.state().vf[0] == kVf0, context + " preserves VF0");
    }
  }
}

void test_fmand_branch(ps2vita::Memory& memory, bool sets_negative) {
  // The SUB result is visible to FMAND in pair 4, four cycles after pair 0.
  // IBNE in pair 5 consumes that FMAND value. Its delay pair must execute,
  // while pair 7 must be skipped only for the negative result.
  for (unsigned index = 0; index < 5u; ++index)
    pair(memory, index, fmand(index + 1u),
         index == 0u ? arithmetic(0x2Cu, 0x8u, 0u) : kUpperNop);
  pair(memory, 5u, (0x29u << 25) | (5u << 11) | 2u); // IBNE vi5,vi0,+2
  pair(memory, 6u, iaddiu(6u, 0x111u));
  pair(memory, 7u, iaddiu(7u, 0xBADu));
  pair(memory, 8u, iaddiu(8u, 0x222u), kUpperNop | 0x40000000u);
  pair(memory, 9u, kLowerNop);

  ps2vita::Vu1 vu(memory);
  vu.enable_store_trace(true);
  const std::uint16_t initial_mac = sets_negative ? 0u : 0x80u;
  const std::uint16_t final_mac = sets_negative ? 0x80u : 0u;
  vu.state().mac = initial_mac;
  vu.state().vi[12] = 0x80u;
  for (unsigned reg = 1; reg <= 5u; ++reg) vu.state().vi[reg] = 0xFFFFu;
  vu.state().vf[1][0] = sets_negative ? 0x3F800000u : 0x40000000u;
  vu.state().vf[2][0] = sets_negative ? 0x40000000u : 0x3F800000u;
  vu.start(0u);
  vu.run(4u);
  const std::string context = sets_negative ? "FMAND sets sign" : "FMAND clears sign";
  for (unsigned reg = 1; reg <= 4u; ++reg)
    check_value(vu.state().vi[reg], initial_mac,
                context + " old flag in slot " + std::to_string(reg - 1u));
  vu.run(1u);
  check_value(vu.state().vi[5], final_mac, context + " new flag in slot 4");
  check_value(vu.flag_read_records().size(), 5u, context + " trace count");
  if (vu.flag_read_records().size() == 5u) {
    for (unsigned index = 0; index < 5u; ++index) {
      const auto& record = vu.flag_read_records()[index];
      check_value(record.mac, index == 4u ? final_mac : initial_mac,
                  context + " traced MAC slot " + std::to_string(index));
      check_value(record.result, index == 4u ? final_mac : initial_mac,
                  context + " traced result slot " + std::to_string(index));
      check_value(record.cycle, index, context + " issue cycle");
    }
  }
  vu.run(20u);
  check_value(vu.state().vi[6], 0x111u, context + " IBNE delay pair");
  check_value(vu.state().vi[7], sets_negative ? 0u : 0xBADu,
              context + " IBNE path marker");
  check_value(vu.state().vi[8], 0x222u, context + " reaches end marker");
  check_value(vu.pairs_executed(), sets_negative ? 9u : 10u,
              context + " branch pair count");
  check_value(vu.state().pc, 80u, context + " final PC");
  check_value(vu.vf_stall_cycles(), 0u, context + " no vector stall hides flag latency");
  check(!vu.running(), context + " E-bit delay pair terminates");
  check(vu.state().vf[0] == kVf0, context + " preserves VF0");
}
} // namespace

int main() {
  ps2vita::Memory memory;
  test_fmand_branch(memory, true);
  test_fmand_branch(memory, false);
  test_arithmetic_destinations(memory);
  test_accumulator_controls(memory);
  test_max_preserves_mac(memory);
  if (failures != 0u) {
    std::cerr << failures << " failures in " << checks << " VU1 flag checks\n";
    return 1;
  }
  std::cout << checks << " VU1 flag checks passed\n";
  return 0;
}
