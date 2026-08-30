#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ps2vita {

class Memory;

enum EeInstructionFlags : std::uint8_t {
  EeNone = 0,
  EeBranch = 1u << 0,
  EeDelaySlot = 1u << 1,
  EeMemory = 1u << 2,
  EeCoprocessor = 1u << 3,
  EeStop = 1u << 4,
};

struct EeDecodedInstruction {
  std::uint32_t pc = 0;
  std::uint32_t opcode = 0;
  std::uint8_t flags = EeNone;
};

struct EeDecodedBlock {
  static constexpr std::size_t kMaxInstructions = 32;

  std::array<EeDecodedInstruction, kMaxInstructions> instructions{};
  std::uint32_t start_pc = 0;
  std::uint32_t source_generation = 0;
  std::uint32_t executions = 0;
  std::uint8_t instruction_count = 0;
  bool valid = false;
};

// A small direct-mapped cache chosen deliberately for Vita. It discovers and
// validates block boundaries now; threaded-interpreter and ARMv7 backends can
// consume the same blocks later without changing architectural CPU semantics.
class EeBlockCache {
public:
  static constexpr std::size_t kSlotCount = 256;

  const EeDecodedBlock& lookup(const Memory& memory, std::uint32_t pc);
  void clear();
  std::size_t resident_blocks() const;

private:
  static EeDecodedBlock decode(const Memory& memory, std::uint32_t pc);
  std::array<EeDecodedBlock, kSlotCount> slots_{};
};

} // namespace ps2vita
