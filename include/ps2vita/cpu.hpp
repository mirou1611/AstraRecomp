#pragma once

#include "ps2vita/ee_block.hpp"
#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>

namespace ps2vita {

enum class StopReason {
  None,
  Halted,
  Break,
  Syscall,
  InvalidInstruction,
  MemoryFault,
  StepLimit,
};

struct CpuState {
  // Low 64 bits are kept in gpr for convenient scalar access. gpr_hi stores
  // bits 64..127 used by EE quadword and multimedia instructions.
  std::array<std::uint64_t, 32> gpr{};
  std::array<std::uint64_t, 32> gpr_hi{};
  std::array<std::uint32_t, 32> cop0{};
  std::array<std::uint32_t, 32> fpr{};
  std::array<std::uint32_t, 32> fcr{};
  std::uint32_t fpu_acc = 0;
  // VU0 vector/control storage used by EE COP2 macro-mode transfers.  Keeping
  // vectors split into host-friendly 64-bit halves matches the EE GPR layout.
  std::array<std::uint64_t, 32> vu0_vf{};
  std::array<std::uint64_t, 32> vu0_vf_hi{};
  std::array<std::uint32_t, 32> vu0_vi{};
  std::uint64_t hi = 0;
  std::uint64_t lo = 0;
  std::uint64_t hi1 = 0;
  std::uint64_t lo1 = 0;
  std::uint32_t pc = 0;
  std::uint64_t cycles = 0;
  // Guest instructions completed by verified native semantic fast paths.
  // They are included in cycles; this counter is diagnostic only.
  std::uint64_t fast_path_instructions = 0;
};

class Cpu {
public:
  explicit Cpu(Memory& memory);

  void reset(std::uint32_t entry);
  StopReason step();
  StopReason run(std::uint32_t instruction_budget);
  void request_halt();
  void set_exception_mode(bool enabled) { exception_mode_ = enabled; }

  const CpuState& state() const { return state_; }
  CpuState& state() { return state_; }
  StopReason stop_reason() const { return stop_reason_; }
  std::uint32_t fault_instruction() const { return fault_instruction_; }
  std::uint32_t fault_address() const { return fault_address_; }

private:
  void branch(std::uint32_t target);
  void set_reg(unsigned index, std::uint64_t value);
  void raise_exception(StopReason reason, std::uint32_t fault_pc,
                       bool in_delay_slot);
  void raise_interrupt(std::uint32_t lines);
  StopReason memory_fault(std::uint32_t address);
  std::uint32_t try_fast_zero_fill(std::uint32_t instruction_budget);
  StopReason execute(std::uint32_t instruction, std::uint32_t current_pc,
                     bool& schedules_branch, std::uint32_t& branch_target,
                     bool& skips_delay_slot);

  Memory& memory_;
  CpuState state_{};
  StopReason stop_reason_ = StopReason::None;
  std::uint32_t fault_instruction_ = 0;
  std::uint32_t fault_address_ = 0;
  bool branch_pending_ = false;
  std::uint32_t pending_target_ = 0;
  bool halt_requested_ = false;
  bool exception_mode_ = false;
  EeBlockCache block_cache_{};
};

const char* stop_reason_name(StopReason reason);

} // namespace ps2vita
