#include "ps2vita/cpu.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

namespace ps2vita {
namespace {

std::uint64_t sx16(std::uint32_t value) {
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(
      static_cast<std::int16_t>(value & 0xFFFFu)));
}

std::uint64_t sx32(std::uint32_t value) {
  return static_cast<std::uint64_t>(static_cast<std::int64_t>(
      static_cast<std::int32_t>(value)));
}

std::uint32_t effective(std::uint64_t base, std::uint32_t instruction) {
  return static_cast<std::uint32_t>(base + sx16(instruction));
}

float as_float(std::uint32_t bits) {
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint32_t as_bits(float value) {
  std::uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

std::uint32_t vu_lane(const CpuState& state, unsigned reg, unsigned index) {
  const auto half = index < 2 ? state.vu0_vf[reg] : state.vu0_vf_hi[reg];
  return static_cast<std::uint32_t>(half >> ((index & 1u) * 32u));
}

void set_vu_lane(CpuState& state, unsigned reg, unsigned index,
                 std::uint32_t value) {
  auto& half = index < 2 ? state.vu0_vf[reg] : state.vu0_vf_hi[reg];
  const unsigned shift = (index & 1u) * 32u;
  half = (half & ~(std::uint64_t{0xFFFFFFFFu} << shift)) |
         (static_cast<std::uint64_t>(value) << shift);
}

std::uint64_t packed_add_unsigned_words(std::uint64_t lhs, std::uint64_t rhs) {
  std::uint64_t result = 0;
  for (unsigned lane = 0; lane < 2; ++lane) {
    const auto a = static_cast<std::uint32_t>(lhs >> (lane * 32));
    const auto b = static_cast<std::uint32_t>(rhs >> (lane * 32));
    const auto sum = static_cast<std::uint64_t>(a) + b;
    const auto saturated = static_cast<std::uint32_t>(
        sum > std::numeric_limits<std::uint32_t>::max()
            ? std::numeric_limits<std::uint32_t>::max() : sum);
    result |= static_cast<std::uint64_t>(saturated) << (lane * 32);
  }
  return result;
}

} // namespace

Cpu::Cpu(Memory& memory) : memory_(memory) {}

void Cpu::reset(std::uint32_t entry) {
  memory_.clear_tlb();
  state_ = {};
  // VF0 is hard-wired to (0, 0, 0, 1); VI0 is hard-wired to zero.
  state_.vu0_vf_hi[0] = 0x3F80000000000000ull;
  state_.pc = entry;
  stop_reason_ = StopReason::None;
  fault_instruction_ = 0;
  fault_address_ = 0;
  branch_pending_ = false;
  pending_target_ = 0;
  halt_requested_ = false;
  exception_mode_ = false;
  block_cache_.clear();
}

void Cpu::request_halt() { halt_requested_ = true; }

void Cpu::set_reg(unsigned index, std::uint64_t value) {
  if (index != 0) state_.gpr[index] = value;
}

void Cpu::branch(std::uint32_t target) {
  branch_pending_ = true;
  pending_target_ = target;
}

StopReason Cpu::step() {
  if (halt_requested_) return stop_reason_ = StopReason::Halted;
  const auto interrupt_lines = memory_.ee_interrupt_lines();
  state_.cop0[13] = (state_.cop0[13] & ~0x00000C00u) | interrupt_lines;
  const auto status = state_.cop0[12];
  if (exception_mode_ && interrupt_lines != 0u &&
      (status & interrupt_lines) != 0u && (status & 0x00010001u) == 0x00010001u &&
      (status & 0x00000006u) == 0u) {
    raise_interrupt(interrupt_lines);
    ++state_.cycles;
    ++state_.cop0[9];
    memory_.advance(1);
    return stop_reason_ = StopReason::None;
  }
  if ((state_.pc & 3u) != 0 || !memory_.valid(state_.pc, 4)) {
    fault_address_ = state_.pc;
    return stop_reason_ = StopReason::MemoryFault;
  }

  const std::uint32_t current_pc = state_.pc;
  const std::uint32_t instruction = memory_.read32(current_pc);
  const bool applies_pending_branch = branch_pending_;
  const std::uint32_t old_target = pending_target_;
  if (instruction == 0x42000018u) { // ERET
    if (state_.cop0[12] & 4u) { // ERL returns through ErrorEPC.
      state_.cop0[12] &= ~4u;
      state_.pc = state_.cop0[30];
    } else {
      state_.cop0[12] &= ~2u;
      state_.pc = state_.cop0[14];
    }
    state_.gpr[0] = 0;
    state_.gpr_hi[0] = 0;
    branch_pending_ = false;
    ++state_.cycles;
    ++state_.cop0[9];
    memory_.advance(1);
    return stop_reason_ = StopReason::None;
  }
  bool schedules_branch = false;
  bool skips_delay_slot = false;
  std::uint32_t new_target = 0;

  const StopReason reason = execute(instruction, current_pc, schedules_branch,
                                    new_target, skips_delay_slot);
  state_.gpr[0] = 0;
  state_.gpr_hi[0] = 0;
  ++state_.cycles;
  ++state_.cop0[9];
  memory_.advance(1);
  if (reason != StopReason::None) {
    fault_instruction_ = instruction;
    if (exception_mode_ && (reason == StopReason::Syscall ||
        reason == StopReason::Break || reason == StopReason::MemoryFault)) {
      raise_exception(reason, current_pc, applies_pending_branch);
      return stop_reason_ = StopReason::None;
    }
    return stop_reason_ = reason;
  }

  branch_pending_ = false;
  state_.pc = current_pc + (skips_delay_slot ? 8u : 4u);
  if (applies_pending_branch) {
    state_.pc = old_target;
  } else if (schedules_branch) {
    branch(new_target);
  }
  return stop_reason_ = StopReason::None;
}

void Cpu::raise_interrupt(std::uint32_t lines) {
  state_.cop0[13] = (state_.cop0[13] & ~0x8000007Cu) | lines;
  state_.cop0[14] = state_.pc;
  state_.cop0[12] |= 2u;
  bool bootstrap_vectors = (state_.cop0[12] & (1u << 22)) != 0;
  if (!bootstrap_vectors) {
    bootstrap_vectors = memory_.read32(0x80000180u) == 0 &&
                        memory_.read32(0x80000184u) == 0 &&
                        memory_.read32(0x80000188u) == 0 &&
                        memory_.read32(0x8000018Cu) == 0;
  }
  state_.pc = bootstrap_vectors ? 0xBFC00200u : 0x80000180u;
  branch_pending_ = false;
}

void Cpu::raise_exception(StopReason reason, std::uint32_t fault_pc,
                          bool in_delay_slot) {
  std::uint32_t code = 0;
  if (reason == StopReason::MemoryFault) code = 4; // AdEL baseline
  else if (reason == StopReason::Syscall) code = 8;
  else if (reason == StopReason::Break) code = 9;
  state_.cop0[13] = code << 2;
  if (in_delay_slot) state_.cop0[13] |= 0x80000000u;
  state_.cop0[14] = in_delay_slot ? fault_pc - 4u : fault_pc;
  if (reason == StopReason::MemoryFault) state_.cop0[8] = fault_address_;
  state_.cop0[12] |= 2u; // EXL
  bool bootstrap_vectors = (state_.cop0[12] & (1u << 22)) != 0;
  if (!bootstrap_vectors) {
    // During early bring-up, the BIOS can prepare/lock vectors through cache
    // operations that the baseline interpreter does not model yet. Never run
    // through an all-zero RAM vector; retain ROM exception service until a real
    // vector is visible in guest memory.
    bootstrap_vectors = memory_.read32(0x80000180u) == 0 &&
                        memory_.read32(0x80000184u) == 0 &&
                        memory_.read32(0x80000188u) == 0 &&
                        memory_.read32(0x8000018Cu) == 0;
  }
  state_.pc = bootstrap_vectors ? 0xBFC00200u : 0x80000180u;
  branch_pending_ = false;
}

StopReason Cpu::memory_fault(std::uint32_t address) {
  fault_address_ = address;
  return StopReason::MemoryFault;
}

StopReason Cpu::run(std::uint32_t instruction_budget) {
  // Populate and age decoded metadata at slice boundaries. Execution remains on
  // the reference interpreter until block behavior is differential-tested.
  (void)block_cache_.lookup(memory_, state_.pc);
  for (std::uint32_t i = 0; i < instruction_budget;) {
    const auto fast_instructions = try_fast_zero_fill(instruction_budget - i);
    if (fast_instructions != 0) {
      i += fast_instructions;
      continue;
    }
    const auto result = step();
    if (result != StopReason::None) return result;
    ++i;
  }
  return StopReason::StepLimit;
}

std::uint32_t Cpu::try_fast_zero_fill(std::uint32_t instruction_budget) {
  // Recognize the compiler-generated EE loop below without depending on a BIOS
  // address.  The interpreter remains the fallback if any instruction, register
  // relationship, alignment, or RAM-bound check differs.
  //
  //   sq    zero_value,0(cursor)
  //   addiu cursor,cursor,16
  //   sltu  condition,cursor,end
  //   nop; nop
  //   bne   condition,zero,loop
  //   por   zero_value,zero,zero       # delay slot
  constexpr std::uint32_t kGuestInstructionsPerIteration = 7;
  if (branch_pending_ || instruction_budget < kGuestInstructionsPerIteration ||
      (state_.pc & 3u) != 0 || !memory_.valid(state_.pc, 28))
    return 0;

  const std::uint32_t pc = state_.pc;
  const std::uint32_t sq = memory_.read32(pc);
  const std::uint32_t addiu = memory_.read32(pc + 4u);
  const std::uint32_t sltu = memory_.read32(pc + 8u);
  const std::uint32_t nop0 = memory_.read32(pc + 12u);
  const std::uint32_t nop1 = memory_.read32(pc + 16u);
  const std::uint32_t bne = memory_.read32(pc + 20u);
  const std::uint32_t por = memory_.read32(pc + 24u);

  if ((sq >> 26) != 0x1Fu || (sq & 0xFFFFu) != 0 ||
      (addiu >> 26) != 0x09u || (addiu & 0xFFFFu) != 16u ||
      (sltu >> 26) != 0 || (sltu & 63u) != 0x2Bu ||
      nop0 != 0 || nop1 != 0 || (bne >> 26) != 0x05u ||
      (bne & 0xFFFFu) != 0xFFFAu)
    return 0;

  const unsigned cursor = (sq >> 21) & 31u;
  const unsigned zero_value = (sq >> 16) & 31u;
  const unsigned addiu_rs = (addiu >> 21) & 31u;
  const unsigned addiu_rt = (addiu >> 16) & 31u;
  const unsigned sltu_rs = (sltu >> 21) & 31u;
  const unsigned end = (sltu >> 16) & 31u;
  const unsigned condition = (sltu >> 11) & 31u;
  const unsigned bne_rs = (bne >> 21) & 31u;
  const unsigned bne_rt = (bne >> 16) & 31u;
  const std::uint32_t expected_por = (0x1Cu << 26) |
      (zero_value << 11) | (0x12u << 6) | 0x29u;
  if (cursor == 0 || cursor != addiu_rs || cursor != addiu_rt ||
      cursor != sltu_rs || condition == 0 || condition != bne_rs ||
      bne_rt != 0 || por != expected_por ||
      state_.gpr[zero_value] != 0 || state_.gpr_hi[zero_value] != 0)
    return 0;

  const auto begin_address = static_cast<std::uint32_t>(state_.gpr[cursor]);
  const auto end_address = static_cast<std::uint32_t>(state_.gpr[end]);
  if ((begin_address & 15u) != 0 || (end_address & 15u) != 0 ||
      begin_address >= end_address || end_address > Memory::kRamSize)
    return 0;

  const std::uint32_t remaining_iterations =
      (end_address - begin_address) / 16u;
  const std::uint32_t budget_iterations =
      instruction_budget / kGuestInstructionsPerIteration;
  const std::uint32_t iterations =
      remaining_iterations < budget_iterations ? remaining_iterations
                                                : budget_iterations;
  const std::uint32_t bytes = iterations * 16u;
  if (iterations == 0 || !memory_.zero(begin_address, bytes)) return 0;

  const std::uint32_t next_address = begin_address + bytes;
  set_reg(cursor, sx32(next_address));
  set_reg(condition, next_address < end_address ? 1u : 0u);
  if (zero_value != 0) {
    state_.gpr[zero_value] = 0;
    state_.gpr_hi[zero_value] = 0;
  }
  const std::uint32_t consumed =
      iterations * kGuestInstructionsPerIteration;
  state_.cycles += consumed;
  state_.cop0[9] += consumed;
  memory_.advance(consumed);
  state_.fast_path_instructions += consumed;
  state_.pc = next_address < end_address ? pc : pc + 28u;
  state_.gpr[0] = 0;
  state_.gpr_hi[0] = 0;
  return consumed;
}

StopReason Cpu::execute(std::uint32_t ins, std::uint32_t pc,
                        bool& schedules, std::uint32_t& target,
                        bool& skips_delay) {
  const unsigned op = ins >> 26;
  const unsigned rs = (ins >> 21) & 31u;
  const unsigned rt = (ins >> 16) & 31u;
  const unsigned rd = (ins >> 11) & 31u;
  const unsigned sa = (ins >> 6) & 31u;
  const unsigned fn = ins & 63u;
  const auto rsv = state_.gpr[rs];
  const auto rtv = state_.gpr[rt];
  const auto branch_to = [&](bool take, bool likely = false) {
    if (take) {
      schedules = true;
      target = pc + 4u + static_cast<std::uint32_t>(
          static_cast<std::int32_t>(static_cast<std::int16_t>(ins)) * 4);
    } else if (likely) {
      skips_delay = true;
    }
  };

  switch (op) {
  case 0x00: // SPECIAL
    switch (fn) {
    case 0x00: set_reg(rd, sx32(static_cast<std::uint32_t>(rtv) << sa)); break;
    case 0x02: set_reg(rd, sx32(static_cast<std::uint32_t>(rtv) >> sa)); break;
    case 0x03: set_reg(rd, sx32(static_cast<std::uint32_t>(
        static_cast<std::int32_t>(rtv) >> sa))); break;
    case 0x04: set_reg(rd, sx32(static_cast<std::uint32_t>(rtv) << (rsv & 31u))); break;
    case 0x06: set_reg(rd, sx32(static_cast<std::uint32_t>(rtv) >> (rsv & 31u))); break;
    case 0x07: set_reg(rd, sx32(static_cast<std::uint32_t>(
        static_cast<std::int32_t>(rtv) >> (rsv & 31u)))); break;
    case 0x08: schedules = true; target = static_cast<std::uint32_t>(rsv); break;
    case 0x09:
      set_reg(rd, sx32(pc + 8u));
      schedules = true; target = static_cast<std::uint32_t>(rsv); break;
    case 0x0A: if (rtv == 0) set_reg(rd, rsv); break; // MOVZ
    case 0x0B: if (rtv != 0) set_reg(rd, rsv); break; // MOVN
    case 0x0C: return StopReason::Syscall;
    case 0x0D: return StopReason::Break;
    case 0x0F: break; // SYNC: interpreter memory accesses are already ordered.
    case 0x10: set_reg(rd, state_.hi); break;
    case 0x11: state_.hi = rsv; break;
    case 0x12: set_reg(rd, state_.lo); break;
    case 0x13: state_.lo = rsv; break;
    case 0x14: set_reg(rd, rtv << (rsv & 63u)); break;
    case 0x16: set_reg(rd, rtv >> (rsv & 63u)); break;
    case 0x17: set_reg(rd, static_cast<std::uint64_t>(
        static_cast<std::int64_t>(rtv) >> (rsv & 63u))); break;
    case 0x18: {
      const std::int64_t product = static_cast<std::int64_t>(
          static_cast<std::int32_t>(rsv)) * static_cast<std::int32_t>(rtv);
      state_.lo = sx32(static_cast<std::uint32_t>(product));
      state_.hi = sx32(static_cast<std::uint32_t>(static_cast<std::uint64_t>(product) >> 32));
      // R5900's three-operand MULT also exposes the low word through rd.
      set_reg(rd, state_.lo);
      break;
    }
    case 0x19: {
      const std::uint64_t product = static_cast<std::uint32_t>(rsv) *
                                    static_cast<std::uint64_t>(static_cast<std::uint32_t>(rtv));
      state_.lo = sx32(static_cast<std::uint32_t>(product));
      state_.hi = sx32(static_cast<std::uint32_t>(product >> 32));
      set_reg(rd, state_.lo);
      break;
    }
    case 0x1A: {
      const auto a = static_cast<std::int32_t>(rsv);
      const auto b = static_cast<std::int32_t>(rtv);
      if (b == 0) { state_.lo = a < 0 ? 1 : std::numeric_limits<std::uint64_t>::max(); state_.hi = sx32(a); }
      else if (a == std::numeric_limits<std::int32_t>::min() && b == -1) { state_.lo = sx32(a); state_.hi = 0; }
      else { state_.lo = sx32(static_cast<std::uint32_t>(a / b)); state_.hi = sx32(static_cast<std::uint32_t>(a % b)); }
      break;
    }
    case 0x1B: {
      const auto a = static_cast<std::uint32_t>(rsv);
      const auto b = static_cast<std::uint32_t>(rtv);
      state_.lo = b ? sx32(a / b) : std::numeric_limits<std::uint64_t>::max();
      state_.hi = sx32(b ? a % b : a);
      break;
    }
    case 0x20: case 0x21: set_reg(rd, sx32(static_cast<std::uint32_t>(rsv + rtv))); break;
    case 0x22: case 0x23: set_reg(rd, sx32(static_cast<std::uint32_t>(rsv - rtv))); break;
    case 0x24: set_reg(rd, rsv & rtv); break;
    case 0x25: set_reg(rd, rsv | rtv); break;
    case 0x26: set_reg(rd, rsv ^ rtv); break;
    case 0x27: set_reg(rd, ~(rsv | rtv)); break;
    case 0x2A: set_reg(rd, static_cast<std::int64_t>(rsv) < static_cast<std::int64_t>(rtv)); break;
    case 0x2B: set_reg(rd, rsv < rtv); break;
    // Overflow exceptions are not exposed yet, so trapping and non-trapping
    // 64-bit forms currently share the same wraparound arithmetic semantics.
    case 0x2C: case 0x2D: // DADD/DADDU
      if (sa != 0) return StopReason::InvalidInstruction;
      set_reg(rd, rsv + rtv); break;
    case 0x2E: case 0x2F: // DSUB/DSUBU
      if (sa != 0) return StopReason::InvalidInstruction;
      set_reg(rd, rsv - rtv); break;
    case 0x38: set_reg(rd, rtv << sa); break;
    case 0x3A: set_reg(rd, rtv >> sa); break;
    case 0x3B: set_reg(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(rtv) >> sa)); break;
    case 0x3C: set_reg(rd, rtv << (sa + 32)); break;
    case 0x3E: set_reg(rd, rtv >> (sa + 32)); break;
    case 0x3F: set_reg(rd, static_cast<std::uint64_t>(static_cast<std::int64_t>(rtv) >> (sa + 32))); break;
    default: return StopReason::InvalidInstruction;
    }
    break;

  case 0x01: { // REGIMM
    const auto signed_rs = static_cast<std::int64_t>(rsv);
    bool take = false;
    bool likely = false;
    bool link = false;
    switch (rt) {
    case 0x00: take = signed_rs < 0; break;
    case 0x01: take = signed_rs >= 0; break;
    case 0x02: take = signed_rs < 0; likely = true; break;
    case 0x03: take = signed_rs >= 0; likely = true; break;
    case 0x10: take = signed_rs < 0; link = true; break;
    case 0x11: take = signed_rs >= 0; link = true; break;
    case 0x12: take = signed_rs < 0; link = true; likely = true; break;
    case 0x13: take = signed_rs >= 0; link = true; likely = true; break;
    default: return StopReason::InvalidInstruction;
    }
    if (link) set_reg(31, sx32(pc + 8u));
    branch_to(take, likely);
    break;
  }
  case 0x02: schedules = true; target = ((pc + 4u) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2); break;
  case 0x03:
    set_reg(31, sx32(pc + 8u)); schedules = true;
    target = ((pc + 4u) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2); break;
  case 0x04: branch_to(rsv == rtv); break;
  case 0x05: branch_to(rsv != rtv); break;
  case 0x06: branch_to(static_cast<std::int64_t>(rsv) <= 0); break;
  case 0x07: branch_to(static_cast<std::int64_t>(rsv) > 0); break;
  case 0x08: case 0x09: set_reg(rt, sx32(static_cast<std::uint32_t>(rsv + sx16(ins)))); break;
  case 0x0A: set_reg(rt, static_cast<std::int64_t>(rsv) < static_cast<std::int64_t>(sx16(ins))); break;
  case 0x0B: set_reg(rt, rsv < sx16(ins)); break;
  case 0x0C: set_reg(rt, rsv & (ins & 0xFFFFu)); break;
  case 0x0D: set_reg(rt, rsv | (ins & 0xFFFFu)); break;
  case 0x0E: set_reg(rt, rsv ^ (ins & 0xFFFFu)); break;
  case 0x0F: set_reg(rt, sx32((ins & 0xFFFFu) << 16)); break;
  case 0x10: // COP0: enough for simple bare-metal startup code.
    if (rs == 0x00) set_reg(rt, sx32(state_.cop0[rd]));
    else if (rs == 0x04) state_.cop0[rd] = static_cast<std::uint32_t>(rtv);
    else if (rs == 0x10 && (fn == 0x38 || fn == 0x39)) { // EI / DI
      constexpr std::uint32_t kEie = 1u << 16;
      constexpr std::uint32_t kEdi = 1u << 17;
      constexpr std::uint32_t kExlErl = (1u << 1) | (1u << 2);
      constexpr std::uint32_t kKsu = 3u << 3;
      auto& status = state_.cop0[12];
      if ((status & kEdi) != 0u || (status & kExlErl) != 0u ||
          (status & kKsu) == 0u) {
        if (fn == 0x38) status |= kEie;
        else status &= ~kEie;
      }
    }
    else if (rs == 0x10 && fn == 0x01) { // TLBR
      std::uint32_t mask = 0, hi = 0, lo0 = 0, lo1 = 0;
      if (memory_.read_tlb(state_.cop0[0] & 0x3Fu, mask, hi, lo0, lo1)) {
        state_.cop0[5] = mask; state_.cop0[10] = hi;
        state_.cop0[2] = lo0; state_.cop0[3] = lo1;
      }
    }
    else if (rs == 0x10 && fn == 0x02) { // TLBWI
      memory_.write_tlb(state_.cop0[0] & 0x3Fu, state_.cop0[5], state_.cop0[10],
                        state_.cop0[2], state_.cop0[3]);
    }
    else if (rs == 0x10 && fn == 0x06) { // TLBWR
      memory_.write_tlb(state_.cop0[1] % 48u, state_.cop0[5], state_.cop0[10],
                        state_.cop0[2], state_.cop0[3]);
    }
    else if (rs == 0x10 && fn == 0x08) { // TLBP
      const int index = memory_.probe_tlb(state_.cop0[10]);
      state_.cop0[0] = index < 0 ? 0x80000000u : static_cast<std::uint32_t>(index);
    }
    else return StopReason::InvalidInstruction;
    break;
  case 0x11: { // COP1 scalar floating point baseline.
    const unsigned fs = rd;
    const unsigned fd = sa;
    const auto set_condition = [&](bool value) {
      constexpr std::uint32_t condition = 1u << 23;
      state_.fcr[31] = value ? (state_.fcr[31] | condition)
                             : (state_.fcr[31] & ~condition);
    };
    if (rs == 0x00) { // MFC1
      set_reg(rt, sx32(state_.fpr[fs]));
    } else if (rs == 0x02) { // CFC1
      set_reg(rt, sx32(state_.fcr[fs]));
    } else if (rs == 0x04) { // MTC1
      state_.fpr[fs] = static_cast<std::uint32_t>(rtv);
    } else if (rs == 0x06) { // CTC1
      state_.fcr[fs] = static_cast<std::uint32_t>(rtv);
    } else if (rs == 0x08) { // BC1F/T/FL/TL
      const bool condition = (state_.fcr[31] & (1u << 23)) != 0;
      const bool take = (rt & 1u) ? condition : !condition;
      branch_to(take, (rt & 2u) != 0);
    } else if (rs == 0x10) { // fmt=S
      const float s = as_float(state_.fpr[fs]);
      const float t = as_float(state_.fpr[rt]);
      switch (fn) {
      case 0x00: state_.fpr[fd] = as_bits(s + t); break;
      case 0x01: state_.fpr[fd] = as_bits(s - t); break;
      case 0x02: state_.fpr[fd] = as_bits(s * t); break;
      case 0x03: state_.fpr[fd] = as_bits(s / t); break;
      case 0x04: state_.fpr[fd] = as_bits(std::sqrt(s)); break;
      case 0x05: state_.fpr[fd] = state_.fpr[fs] & 0x7FFFFFFFu; break;
      case 0x06: state_.fpr[fd] = state_.fpr[fs]; break;
      case 0x07: state_.fpr[fd] = state_.fpr[fs] ^ 0x80000000u; break;
      case 0x18: state_.fpu_acc = as_bits(s + t); break; // ADDA.S
      case 0x19: state_.fpu_acc = as_bits(s - t); break; // SUBA.S
      case 0x1A: state_.fpu_acc = as_bits(s * t); break; // MULA.S
      case 0x1C: // MADD.S
        state_.fpr[fd] = as_bits(as_float(state_.fpu_acc) + s * t); break;
      case 0x1D: // MSUB.S
        state_.fpr[fd] = as_bits(as_float(state_.fpu_acc) - s * t); break;
      case 0x1E: // MADDA.S
        state_.fpu_acc = as_bits(as_float(state_.fpu_acc) + s * t); break;
      case 0x1F: // MSUBA.S
        state_.fpu_acc = as_bits(as_float(state_.fpu_acc) - s * t); break;
      case 0x24:
        if (!std::isfinite(s) || static_cast<double>(s) >= 2147483648.0 ||
            static_cast<double>(s) < -2147483648.0) {
          state_.fpr[fd] = 0x80000000u;
        } else {
          state_.fpr[fd] = static_cast<std::uint32_t>(static_cast<std::int32_t>(s));
        }
        break;
      case 0x28: state_.fpr[fd] = as_bits(std::fmax(s, t)); break;
      case 0x29: state_.fpr[fd] = as_bits(std::fmin(s, t)); break;
      case 0x30: set_condition(false); break;
      case 0x32: set_condition(!std::isnan(s) && !std::isnan(t) && s == t); break;
      case 0x3C: set_condition(!std::isnan(s) && !std::isnan(t) && s < t); break;
      case 0x3E: set_condition(!std::isnan(s) && !std::isnan(t) && s <= t); break;
      default: return StopReason::InvalidInstruction;
      }
    } else if (rs == 0x14 && fn == 0x20) { // CVT.S.W
      state_.fpr[fd] = as_bits(static_cast<float>(
          static_cast<std::int32_t>(state_.fpr[fs])));
    } else {
      return StopReason::InvalidInstruction;
    }
    break;
  }
  case 0x12: { // COP2 / VU0 macro-mode transfers.
    constexpr unsigned kR = 20;
    constexpr unsigned kMac = 17;
    constexpr unsigned kTpc = 26;
    constexpr unsigned kFbrst = 28;
    constexpr unsigned kVpuStat = 29;
    constexpr unsigned kCmsar1 = 31;
    if (rs == 0x00 || rs == 0x01) { // QMFC2, optional interlock bit
      if (rt != 0) {
        state_.gpr[rt] = state_.vu0_vf[rd];
        state_.gpr_hi[rt] = state_.vu0_vf_hi[rd];
      }
    } else if (rs == 0x02) { // CFC2
      if (rd == kR) {
        // R exposes 23 mantissa bits and only replaces the low GPR word.
        if (rt != 0) {
          state_.gpr[rt] = (state_.gpr[rt] & 0xFFFFFFFF00000000ull) |
                           (state_.vu0_vi[kR] & 0x007FFFFFu);
        }
      } else {
        set_reg(rt, sx32(state_.vu0_vi[rd]));
      }
    } else if (rs == 0x04 || rs == 0x05) { // QMTC2, optional interlock bit
      if (rd != 0) {
        state_.vu0_vf[rd] = rtv;
        state_.vu0_vf_hi[rd] = state_.gpr_hi[rt];
      }
    } else if (rs == 0x06) { // CTC2
      const auto value = static_cast<std::uint32_t>(rtv);
      if (rd == 0 || rd == kMac || rd == kTpc || rd == kVpuStat) {
        break; // Hard-wired zero or read-only control registers.
      }
      if (rd == kR) {
        state_.vu0_vi[kR] = (value & 0x007FFFFFu) | 0x3F800000u;
      } else if (rd == kFbrst) {
        state_.vu0_vi[kFbrst] = value & 0x00000C0Cu;
        if (value & 0x2u) {
          state_.vu0_vf = {};
          state_.vu0_vf_hi = {};
          state_.vu0_vi = {};
          state_.vu0_vf_hi[0] = 0x3F80000000000000ull;
        }
      } else if (rd == kCmsar1) {
        // VU1 micro execution is introduced with the VIF/VU1 subsystem.
        state_.vu0_vi[kCmsar1] = value;
      } else {
        state_.vu0_vi[rd] = value;
      }
    } else if (rs >= 0x10) { // VU0 macro-mode arithmetic/special encoding.
      const unsigned special2 = (ins & 3u) | ((ins >> 4) & 0x7Cu);
      if (fn == 0x30u || fn == 0x31u || fn == 0x32u ||
          fn == 0x34u || fn == 0x35u) { // VIADD/VISUB/VIADDI/VIAND/VIOR
        const unsigned it = rt & 15u;
        const unsigned is = rd & 15u;
        const unsigned id = sa & 15u;
        unsigned destination = id;
        std::uint16_t result = 0;
        const auto lhs = static_cast<std::uint16_t>(state_.vu0_vi[is]);
        const auto rhs = static_cast<std::uint16_t>(state_.vu0_vi[it]);
        if (fn == 0x30u) result = static_cast<std::uint16_t>(lhs + rhs);
        else if (fn == 0x31u) result = static_cast<std::uint16_t>(lhs - rhs);
        else if (fn == 0x32u) {
          destination = it;
          const auto immediate = static_cast<std::int16_t>(
              static_cast<std::int8_t>((sa & 0x10u) ? (sa | 0xE0u) : sa));
          result = static_cast<std::uint16_t>(lhs + immediate);
        } else if (fn == 0x34u) result = lhs & rhs;
        else result = lhs | rhs;
        if (destination != 0) {
          state_.vu0_vi[destination] =
              (state_.vu0_vi[destination] & 0xFFFF0000u) | result;
        }
      } else if (fn == 0x2Cu) { // VSUB
        const unsigned ft = rt;
        const unsigned fs = rd;
        const unsigned fd = sa;
        if (fd != 0) {
          for (unsigned index = 0; index < 4; ++index) {
            if (ins & (1u << (24u - index))) {
              set_vu_lane(state_, fd, index,
                          as_bits(as_float(vu_lane(state_, fs, index)) -
                                  as_float(vu_lane(state_, ft, index))));
            }
          }
        }
      } else if (fn >= 0x3Cu && special2 >= 0x34u &&
                 special2 <= 0x37u) { // VLQI/VSQI/VLQD/VSQD
        const bool store = (special2 & 1u) != 0;
        const bool decrement = (special2 & 2u) != 0;
        const unsigned vector_reg = store ? rd : rt;
        const unsigned address_reg = (store ? rt : rd) & 15u;
        auto vi16 = static_cast<std::uint16_t>(state_.vu0_vi[address_reg]);
        if (decrement && address_reg != 0) --vi16;
        const unsigned qword = vi16 & 0xFFu;
        if (store) {
          for (unsigned lane = 0; lane < 4; ++lane) {
            if (ins & (1u << (24u - lane))) {
              memory_.write32(Memory::kVu0DataBase + qword * 16u + lane * 4u,
                              vu_lane(state_, vector_reg, lane));
            }
          }
        } else if (vector_reg != 0) {
          for (unsigned lane = 0; lane < 4; ++lane) {
            if (ins & (1u << (24u - lane))) {
              set_vu_lane(state_, vector_reg, lane,
                          memory_.read32(Memory::kVu0DataBase +
                                         qword * 16u + lane * 4u));
            }
          }
        }
        if (!decrement && address_reg != 0) ++vi16;
        if (address_reg != 0) {
          state_.vu0_vi[address_reg] =
              (state_.vu0_vi[address_reg] & 0xFFFF0000u) | vi16;
        }
      } else if (fn >= 0x3Cu && special2 == 0x3Fu) { // VISWR
        const unsigned it = rt & 15u;
        const unsigned is = rd & 15u;
        const unsigned qword = state_.vu0_vi[is] & 0xFFu;
        const std::uint32_t value = state_.vu0_vi[it] & 0xFFFFu;
        for (unsigned lane = 0; lane < 4; ++lane) {
          // Destination lane mask is encoded X,Y,Z,W from high to low.
          if (ins & (1u << (24u - lane))) {
            memory_.write32(Memory::kVu0DataBase + qword * 16u + lane * 4u,
                            value);
          }
        }
      } else {
        return StopReason::InvalidInstruction;
      }
    } else {
      return StopReason::InvalidInstruction;
    }
    break;
  }
  case 0x14: branch_to(rsv == rtv, true); break;
  case 0x15: branch_to(rsv != rtv, true); break;
  case 0x16: branch_to(static_cast<std::int64_t>(rsv) <= 0, true); break;
  case 0x17: branch_to(static_cast<std::int64_t>(rsv) > 0, true); break;
  case 0x18: case 0x19: set_reg(rt, rsv + sx16(ins)); break;
  case 0x1A: { // LDL (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~7u;
    if (!memory_.valid(aligned, 8)) return memory_fault(a);
    const unsigned shift = (7u - (a & 7u)) * 8u;
    const std::uint64_t keep = shift ? ((std::uint64_t{1} << shift) - 1u) : 0u;
    set_reg(rt, (rtv & keep) | (memory_.read64(aligned) << shift)); break;
  }
  case 0x1B: { // LDR (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~7u;
    if (!memory_.valid(aligned, 8)) return memory_fault(a);
    const unsigned shift = (a & 7u) * 8u;
    const std::uint64_t keep = shift ? (~std::uint64_t{0} << (64u - shift)) : 0u;
    set_reg(rt, (rtv & keep) | (memory_.read64(aligned) >> shift)); break;
  }
  case 0x1C: { // MMI multimedia instruction groups.
    const unsigned sub = (ins >> 6) & 31u;
    if (fn == 0x10) { // MFHI1
      set_reg(rd, state_.hi1);
    } else if (fn == 0x11) { // MTHI1
      state_.hi1 = rsv;
    } else if (fn == 0x12) { // MFLO1
      set_reg(rd, state_.lo1);
    } else if (fn == 0x13) { // MTLO1
      state_.lo1 = rsv;
    } else if (fn == 0x18) { // MULT1
      const std::int64_t product = static_cast<std::int64_t>(
          static_cast<std::int32_t>(rsv)) * static_cast<std::int32_t>(rtv);
      state_.lo1 = sx32(static_cast<std::uint32_t>(product));
      state_.hi1 = sx32(static_cast<std::uint32_t>(
          static_cast<std::uint64_t>(product) >> 32));
      set_reg(rd, state_.lo1);
    } else if (fn == 0x19) { // MULTU1
      const std::uint64_t product = static_cast<std::uint32_t>(rsv) *
                                    static_cast<std::uint64_t>(
                                        static_cast<std::uint32_t>(rtv));
      state_.lo1 = sx32(static_cast<std::uint32_t>(product));
      state_.hi1 = sx32(static_cast<std::uint32_t>(product >> 32));
      set_reg(rd, state_.lo1);
    } else if (fn == 0x1A) { // DIV1
      const auto a = static_cast<std::int32_t>(rsv);
      const auto b = static_cast<std::int32_t>(rtv);
      if (b == 0) {
        state_.lo1 = a < 0 ? 1 : std::numeric_limits<std::uint64_t>::max();
        state_.hi1 = sx32(a);
      } else if (a == std::numeric_limits<std::int32_t>::min() && b == -1) {
        state_.lo1 = sx32(a); state_.hi1 = 0;
      } else {
        state_.lo1 = sx32(static_cast<std::uint32_t>(a / b));
        state_.hi1 = sx32(static_cast<std::uint32_t>(a % b));
      }
    } else if (fn == 0x1B) { // DIVU1
      const auto a = static_cast<std::uint32_t>(rsv);
      const auto b = static_cast<std::uint32_t>(rtv);
      state_.lo1 = b ? sx32(a / b) : std::numeric_limits<std::uint64_t>::max();
      state_.hi1 = sx32(b ? a % b : a);
    } else if (fn == 0x09 && sub == 0x08) { // PMFHI
      if (rd != 0) {
        state_.gpr[rd] = state_.hi;
        state_.gpr_hi[rd] = state_.hi1;
      }
    } else if (fn == 0x09 && sub == 0x09) { // PMFLO
      if (rd != 0) {
        state_.gpr[rd] = state_.lo;
        state_.gpr_hi[rd] = state_.lo1;
      }
    } else if (fn == 0x09 && sub == 0x0E) { // PCPYLD
      if (rd != 0) {
        const auto high = rsv;
        state_.gpr[rd] = rtv;
        state_.gpr_hi[rd] = high;
      }
    } else if (fn == 0x29 && sub == 0x08) { // PMTHI
      state_.hi = rsv;
      state_.hi1 = state_.gpr_hi[rs];
    } else if (fn == 0x29 && sub == 0x09) { // PMTLO
      state_.lo = rsv;
      state_.lo1 = state_.gpr_hi[rs];
    } else if (fn == 0x29 && sub == 0x0E) { // PCPYUD
      if (rd != 0) {
        const auto low = state_.gpr_hi[rs];
        state_.gpr_hi[rd] = state_.gpr_hi[rt];
        state_.gpr[rd] = low;
      }
    } else if (fn == 0x28 && sub == 0x10) { // PADDUW
      if (rd != 0) {
        state_.gpr[rd] = packed_add_unsigned_words(rsv, rtv);
        state_.gpr_hi[rd] = packed_add_unsigned_words(
            state_.gpr_hi[rs], state_.gpr_hi[rt]);
      }
    } else if (fn == 0x29 && sub == 0x12) { // POR
      if (rd != 0) {
        state_.gpr[rd] = rsv | rtv;
        state_.gpr_hi[rd] = state_.gpr_hi[rs] | state_.gpr_hi[rt];
      }
    } else {
      return StopReason::InvalidInstruction;
    }
    break;
  }
  case 0x1E: { // LQ: EE silently aligns quadword addresses.
    const auto aligned = effective(rsv, ins) & ~15u;
    if (!memory_.valid(aligned, 16)) return memory_fault(aligned);
    const auto low = memory_.read64(aligned);
    const auto high = memory_.read64(aligned + 8u);
    if (rt != 0) { state_.gpr[rt] = low; state_.gpr_hi[rt] = high; }
    break;
  }
  case 0x1F: { // SQ: EE silently aligns quadword addresses.
    const auto aligned = effective(rsv, ins) & ~15u;
    if (!memory_.valid(aligned, 16)) return memory_fault(aligned);
    memory_.write64(aligned, state_.gpr[rt]);
    memory_.write64(aligned + 8u, state_.gpr_hi[rt]);
    break;
  }

  case 0x20: { const auto a = effective(rsv, ins); if (!memory_.valid(a)) return memory_fault(a); const auto v = memory_.read8(a); set_reg(rt, sx16(v | ((v & 0x80u) ? 0xFF00u : 0u))); break; }
  case 0x21: { const auto a = effective(rsv, ins); if ((a & 1u) || !memory_.valid(a, 2)) return memory_fault(a); set_reg(rt, sx16(memory_.read16(a))); break; }
  case 0x22: { // LWL (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
    if (!memory_.valid(aligned, 4)) return memory_fault(a);
    const unsigned shift = (3u - (a & 3u)) * 8u;
    const std::uint32_t keep = shift ? ((std::uint32_t{1} << shift) - 1u) : 0u;
    const auto value = (static_cast<std::uint32_t>(rtv) & keep) |
                       (memory_.read32(aligned) << shift);
    set_reg(rt, sx32(value)); break;
  }
  case 0x23: { const auto a = effective(rsv, ins); if ((a & 3u) || !memory_.valid(a, 4)) return memory_fault(a); set_reg(rt, sx32(memory_.read32(a))); break; }
  case 0x24: { const auto a = effective(rsv, ins); if (!memory_.valid(a)) return memory_fault(a); set_reg(rt, memory_.read8(a)); break; }
  case 0x25: { const auto a = effective(rsv, ins); if ((a & 1u) || !memory_.valid(a, 2)) return memory_fault(a); set_reg(rt, memory_.read16(a)); break; }
  case 0x26: { // LWR (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
    if (!memory_.valid(aligned, 4)) return memory_fault(a);
    const unsigned shift = (a & 3u) * 8u;
    const std::uint32_t keep = shift ? (~std::uint32_t{0} << (32u - shift)) : 0u;
    const auto value = (static_cast<std::uint32_t>(rtv) & keep) |
                       (memory_.read32(aligned) >> shift);
    if (shift == 0) set_reg(rt, sx32(value));
    else set_reg(rt, (rtv & 0xFFFFFFFF00000000ull) | value);
    break;
  }
  case 0x27: { const auto a = effective(rsv, ins); if ((a & 3u) || !memory_.valid(a, 4)) return memory_fault(a); set_reg(rt, memory_.read32(a)); break; }
  case 0x28: { const auto a = effective(rsv, ins); if (!memory_.valid(a)) return memory_fault(a); memory_.write8(a, static_cast<std::uint8_t>(rtv)); break; }
  case 0x29: { const auto a = effective(rsv, ins); if ((a & 1u) || !memory_.valid(a, 2)) return memory_fault(a); memory_.write16(a, static_cast<std::uint16_t>(rtv)); break; }
  case 0x2A: { // SWL (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
    if (!memory_.valid(aligned, 4)) return memory_fault(a);
    const unsigned shift = (3u - (a & 3u)) * 8u;
    const std::uint32_t replace = ~std::uint32_t{0} >> shift;
    memory_.write32(aligned, (memory_.read32(aligned) & ~replace) |
        ((static_cast<std::uint32_t>(rtv) >> shift) & replace)); break;
  }
  case 0x2B: { const auto a = effective(rsv, ins); if ((a & 3u) || !memory_.valid(a, 4)) return memory_fault(a); memory_.write32(a, static_cast<std::uint32_t>(rtv)); break; }
  case 0x2C: { // SDL (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~7u;
    if (!memory_.valid(aligned, 8)) return memory_fault(a);
    const unsigned shift = (7u - (a & 7u)) * 8u;
    const std::uint64_t replace = ~std::uint64_t{0} >> shift;
    memory_.write64(aligned, (memory_.read64(aligned) & ~replace) |
        ((rtv >> shift) & replace)); break;
  }
  case 0x2D: { // SDR (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~7u;
    if (!memory_.valid(aligned, 8)) return memory_fault(a);
    const unsigned shift = (a & 7u) * 8u;
    const std::uint64_t replace = ~std::uint64_t{0} << shift;
    memory_.write64(aligned, (memory_.read64(aligned) & ~replace) |
        ((rtv << shift) & replace)); break;
  }
  case 0x2E: { // SWR (little-endian)
    const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
    if (!memory_.valid(aligned, 4)) return memory_fault(a);
    const unsigned shift = (a & 3u) * 8u;
    const std::uint32_t replace = ~std::uint32_t{0} << shift;
    memory_.write32(aligned, (memory_.read32(aligned) & ~replace) |
        ((static_cast<std::uint32_t>(rtv) << shift) & replace)); break;
  }
  case 0x2F: break; // CACHE: no data/instruction caches in the interpreter.
  case 0x31: { // LWC1
    const auto a = effective(rsv, ins);
    if ((a & 3u) || !memory_.valid(a, 4)) return memory_fault(a);
    state_.fpr[rt] = memory_.read32(a);
    break;
  }
  case 0x33: break; // PREF: safe hint to ignore.
  case 0x36: { // LQC2: EE silently aligns VU quadword transfers.
    const auto aligned = effective(rsv, ins) & ~15u;
    if (!memory_.valid(aligned, 16)) return memory_fault(aligned);
    if (rt != 0) {
      state_.vu0_vf[rt] = memory_.read64(aligned);
      state_.vu0_vf_hi[rt] = memory_.read64(aligned + 8u);
    }
    break;
  }
  case 0x39: { // SWC1
    const auto a = effective(rsv, ins);
    if ((a & 3u) || !memory_.valid(a, 4)) return memory_fault(a);
    memory_.write32(a, state_.fpr[rt]);
    break;
  }
  case 0x3E: { // SQC2
    const auto aligned = effective(rsv, ins) & ~15u;
    if (!memory_.valid(aligned, 16)) return memory_fault(aligned);
    memory_.write64(aligned, state_.vu0_vf[rt]);
    memory_.write64(aligned + 8u, state_.vu0_vf_hi[rt]);
    break;
  }
  case 0x37: { const auto a = effective(rsv, ins); if ((a & 7u) || !memory_.valid(a, 8)) return memory_fault(a); set_reg(rt, memory_.read64(a)); break; }
  case 0x3F: { const auto a = effective(rsv, ins); if ((a & 7u) || !memory_.valid(a, 8)) return memory_fault(a); memory_.write64(a, rtv); break; }
  default: return StopReason::InvalidInstruction;
  }
  return StopReason::None;
}

const char* stop_reason_name(StopReason reason) {
  switch (reason) {
  case StopReason::None: return "running";
  case StopReason::Halted: return "halted";
  case StopReason::Break: return "break";
  case StopReason::Syscall: return "syscall";
  case StopReason::InvalidInstruction: return "invalid instruction";
  case StopReason::MemoryFault: return "memory fault";
  case StopReason::StepLimit: return "step limit";
  }
  return "unknown";
}

} // namespace ps2vita
