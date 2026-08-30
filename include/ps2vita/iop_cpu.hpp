#pragma once

#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>

namespace ps2vita {

enum class IopStopReason {
  None,
  InvalidInstruction,
  MemoryFault,
};

struct IopState {
  std::array<std::uint32_t, 32> gpr{};
  std::array<std::uint32_t, 32> cop0{};
  std::uint32_t hi = 0;
  std::uint32_t lo = 0;
  std::uint32_t pc = 0;
  std::uint64_t cycles = 0;
};

class IopCpu {
public:
  explicit IopCpu(Memory& memory);
  void reset();
  IopStopReason step();
  IopStopReason run(std::uint32_t instruction_budget);

  const IopState& state() const { return state_; }
  IopState& state() { return state_; }
  IopStopReason stop_reason() const { return stop_reason_; }
  std::uint32_t fault_instruction() const { return fault_instruction_; }
  std::uint32_t fault_address() const { return fault_address_; }

private:
  void set_reg(unsigned index, std::uint32_t value);
  void schedule_load(unsigned index, std::uint32_t value);
  void raise_exception(std::uint32_t code, std::uint32_t pc, bool delay_slot);
  bool execute(std::uint32_t instruction, std::uint32_t pc,
               bool& schedules_branch, std::uint32_t& branch_target,
               bool& skips_delay_slot, std::uint32_t& exception_code);

  Memory& memory_;
  IopState state_{};
  IopStopReason stop_reason_ = IopStopReason::None;
  std::uint32_t fault_instruction_ = 0;
  std::uint32_t fault_address_ = 0;
  bool branch_pending_ = false;
  std::uint32_t pending_target_ = 0;
  bool load_pending_ = false;
  unsigned pending_load_register_ = 0;
  std::uint32_t pending_load_value_ = 0;
};

const char* iop_stop_reason_name(IopStopReason reason);

} // namespace ps2vita
