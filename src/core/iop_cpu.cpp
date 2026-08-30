#include "ps2vita/iop_cpu.hpp"

#include <cstdint>
#include <limits>

namespace ps2vita {
namespace {

std::uint32_t sx16(std::uint32_t value) {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(
      static_cast<std::int16_t>(value)));
}

std::uint32_t effective(std::uint32_t base, std::uint32_t instruction) {
  return base + sx16(instruction);
}

} // namespace

IopCpu::IopCpu(Memory& memory) : memory_(memory) { reset(); }

void IopCpu::reset() {
  state_ = {};
  state_.pc = 0xBFC00000u;
  state_.cop0[12] = 0x00400000u; // BEV
  state_.cop0[15] = 0x0000001Fu; // R3000A-compatible PRId
  stop_reason_ = IopStopReason::None;
  fault_instruction_ = 0;
  fault_address_ = 0;
  branch_pending_ = false;
  pending_target_ = 0;
  load_pending_ = false;
  pending_load_register_ = 0;
  pending_load_value_ = 0;
}

void IopCpu::set_reg(unsigned index, std::uint32_t value) {
  if (index != 0) state_.gpr[index] = value;
}

void IopCpu::schedule_load(unsigned index, std::uint32_t value) {
  if (index == 0) return;
  load_pending_ = true;
  pending_load_register_ = index;
  pending_load_value_ = value;
}

void IopCpu::raise_exception(std::uint32_t code, std::uint32_t pc,
                             bool delay_slot) {
  state_.cop0[13] = (state_.cop0[13] & ~0x8000007Cu) | (code << 2);
  if (delay_slot) state_.cop0[13] |= 0x80000000u;
  state_.cop0[14] = delay_slot ? pc - 4u : pc;
  state_.cop0[12] = (state_.cop0[12] & ~0x3Fu) |
      ((state_.cop0[12] & 0x0Fu) << 2);
  state_.pc = (state_.cop0[12] & 0x00400000u)
      ? 0xBFC00180u : 0x80000080u;
  branch_pending_ = false;
}

IopStopReason IopCpu::step() {
  const auto pc = state_.pc;
  const bool interrupt_pending = memory_.iop_interrupt_pending();
  if (interrupt_pending) state_.cop0[13] |= 0x00000400u;
  else state_.cop0[13] &= ~0x00000400u;
  if (interrupt_pending && (state_.cop0[12] & 0x00000401u) == 0x00000401u) {
    raise_exception(0, pc, branch_pending_);
    ++state_.cycles;
    stop_reason_ = IopStopReason::None;
    return stop_reason_;
  }
  if ((pc & 3u) != 0 || !memory_.iop_valid(pc, 4)) {
    fault_address_ = pc;
    stop_reason_ = IopStopReason::MemoryFault;
    raise_exception(4, pc, branch_pending_);
    ++state_.cycles;
    return stop_reason_;
  }
  const auto instruction = memory_.iop_read32(pc);
  const bool commits_load = load_pending_;
  const unsigned old_load_register = pending_load_register_;
  const auto old_load_value = pending_load_value_;
  load_pending_ = false;
  const bool applies_pending_branch = branch_pending_;
  const auto old_target = pending_target_;
  bool schedules_branch = false;
  bool skips_delay_slot = false;
  std::uint32_t new_target = 0;
  std::uint32_t exception_code = 0;
  const bool known = execute(instruction, pc, schedules_branch, new_target,
                             skips_delay_slot, exception_code);
  if (commits_load) set_reg(old_load_register, old_load_value);
  state_.gpr[0] = 0;
  ++state_.cycles;
  if (!known || exception_code != 0) {
    fault_instruction_ = instruction;
    stop_reason_ = known ? IopStopReason::None
                         : IopStopReason::InvalidInstruction;
    raise_exception(known ? exception_code : 10u, pc, applies_pending_branch);
    return stop_reason_;
  }
  branch_pending_ = false;
  state_.pc = pc + (skips_delay_slot ? 8u : 4u);
  if (applies_pending_branch) {
    state_.pc = old_target;
  } else if (schedules_branch) {
    branch_pending_ = true;
    pending_target_ = new_target;
  }
  stop_reason_ = IopStopReason::None;
  return stop_reason_;
}

IopStopReason IopCpu::run(std::uint32_t instruction_budget) {
  IopStopReason result = IopStopReason::None;
  for (std::uint32_t i = 0; i < instruction_budget; ++i) result = step();
  return result;
}

bool IopCpu::execute(std::uint32_t ins, std::uint32_t pc, bool& schedules,
                     std::uint32_t& target, bool& skips,
                     std::uint32_t& exception) {
  const unsigned op = ins >> 26;
  const unsigned rs = (ins >> 21) & 31u;
  const unsigned rt = (ins >> 16) & 31u;
  const unsigned rd = (ins >> 11) & 31u;
  const unsigned sa = (ins >> 6) & 31u;
  const unsigned fn = ins & 63u;
  const auto rsv = state_.gpr[rs];
  const auto rtv = state_.gpr[rt];
  // R3000A cache isolation redirects ordinary RAM stores into the data cache.
  // We do not model cache contents yet, but suppressing the backing-RAM write
  // preserves the architecturally visible behavior used by the reset ROM while
  // it invalidates cache lines. Hardware and scratchpad writes remain live.
  const auto write_memory = [&](std::uint32_t address, auto&& writer) {
    const auto physical = address & 0x1FFFFFFFu;
    const bool isolated_ram = (state_.cop0[12] & 0x00010000u) != 0u &&
        physical < Memory::kIopWindowSize;
    if (!isolated_ram) writer();
  };
  const auto branch_to = [&](bool take, bool likely = false) {
    if (take) {
      schedules = true;
      target = pc + 4u + static_cast<std::uint32_t>(
          static_cast<std::int32_t>(static_cast<std::int16_t>(ins)) * 4);
    } else if (likely) {
      skips = true;
    }
  };
  switch (op) {
  case 0x00:
    switch (fn) {
    case 0x00: set_reg(rd, rtv << sa); break;
    case 0x02: set_reg(rd, rtv >> sa); break;
    case 0x03: set_reg(rd, static_cast<std::uint32_t>(
        static_cast<std::int32_t>(rtv) >> sa)); break;
    case 0x04: set_reg(rd, rtv << (rsv & 31u)); break;
    case 0x06: set_reg(rd, rtv >> (rsv & 31u)); break;
    case 0x07: set_reg(rd, static_cast<std::uint32_t>(
        static_cast<std::int32_t>(rtv) >> (rsv & 31u))); break;
    case 0x08: schedules = true; target = rsv; break;
    case 0x09: set_reg(rd ? rd : 31u, pc + 8u); schedules = true; target = rsv; break;
    case 0x0C: exception = 8; break;
    case 0x0D: exception = 9; break;
    case 0x10: set_reg(rd, state_.hi); break;
    case 0x11: state_.hi = rsv; break;
    case 0x12: set_reg(rd, state_.lo); break;
    case 0x13: state_.lo = rsv; break;
    case 0x18: {
      const auto result = static_cast<std::int64_t>(static_cast<std::int32_t>(rsv)) *
          static_cast<std::int64_t>(static_cast<std::int32_t>(rtv));
      state_.lo = static_cast<std::uint32_t>(result);
      state_.hi = static_cast<std::uint32_t>(static_cast<std::uint64_t>(result) >> 32);
      break;
    }
    case 0x19: {
      const auto result = static_cast<std::uint64_t>(rsv) * rtv;
      state_.lo = static_cast<std::uint32_t>(result);
      state_.hi = static_cast<std::uint32_t>(result >> 32);
      break;
    }
    case 0x1A:
      if (rtv != 0) {
        const auto lhs = static_cast<std::int32_t>(rsv);
        const auto rhs = static_cast<std::int32_t>(rtv);
        if (lhs == std::numeric_limits<std::int32_t>::min() && rhs == -1) {
          state_.lo = static_cast<std::uint32_t>(lhs); state_.hi = 0;
        } else {
          state_.lo = static_cast<std::uint32_t>(lhs / rhs);
          state_.hi = static_cast<std::uint32_t>(lhs % rhs);
        }
      } else {
        state_.lo = static_cast<std::int32_t>(rsv) < 0 ? 1u : 0xFFFFFFFFu;
        state_.hi = rsv;
      }
      break;
    case 0x1B:
      if (rtv != 0) { state_.lo = rsv / rtv; state_.hi = rsv % rtv; }
      else { state_.lo = 0xFFFFFFFFu; state_.hi = rsv; }
      break;
    case 0x20: case 0x21: set_reg(rd, rsv + rtv); break;
    case 0x22: case 0x23: set_reg(rd, rsv - rtv); break;
    case 0x24: set_reg(rd, rsv & rtv); break;
    case 0x25: set_reg(rd, rsv | rtv); break;
    case 0x26: set_reg(rd, rsv ^ rtv); break;
    case 0x27: set_reg(rd, ~(rsv | rtv)); break;
    case 0x2A: set_reg(rd, static_cast<std::int32_t>(rsv) <
        static_cast<std::int32_t>(rtv)); break;
    case 0x2B: set_reg(rd, rsv < rtv); break;
    default: return false;
    }
    break;
  case 0x01:
    switch (rt) {
    case 0x00: branch_to(static_cast<std::int32_t>(rsv) < 0); break;
    case 0x01: branch_to(static_cast<std::int32_t>(rsv) >= 0); break;
    case 0x10: set_reg(31, pc + 8u); branch_to(static_cast<std::int32_t>(rsv) < 0); break;
    case 0x11: set_reg(31, pc + 8u); branch_to(static_cast<std::int32_t>(rsv) >= 0); break;
    default: return false;
    }
    break;
  case 0x02: schedules = true; target = ((pc + 4u) & 0xF0000000u) |
      ((ins & 0x03FFFFFFu) << 2); break;
  case 0x03: set_reg(31, pc + 8u); schedules = true;
      target = ((pc + 4u) & 0xF0000000u) | ((ins & 0x03FFFFFFu) << 2); break;
  case 0x04: branch_to(rsv == rtv); break;
  case 0x05: branch_to(rsv != rtv); break;
  case 0x06: branch_to(static_cast<std::int32_t>(rsv) <= 0); break;
  case 0x07: branch_to(static_cast<std::int32_t>(rsv) > 0); break;
  case 0x08: case 0x09: set_reg(rt, rsv + sx16(ins)); break;
  case 0x0A: set_reg(rt, static_cast<std::int32_t>(rsv) <
      static_cast<std::int32_t>(sx16(ins))); break;
  case 0x0B: set_reg(rt, rsv < sx16(ins)); break;
  case 0x0C: set_reg(rt, rsv & (ins & 0xFFFFu)); break;
  case 0x0D: set_reg(rt, rsv | (ins & 0xFFFFu)); break;
  case 0x0E: set_reg(rt, rsv ^ (ins & 0xFFFFu)); break;
  case 0x0F: set_reg(rt, ins << 16); break;
  case 0x10:
    if (rs == 0x00) set_reg(rt, state_.cop0[rd]);
    else if (rs == 0x04) state_.cop0[rd] = rtv;
    else if (rs == 0x10 && fn == 0x10)
      state_.cop0[12] = (state_.cop0[12] & ~0x0Fu) |
          ((state_.cop0[12] & 0x3Cu) >> 2);
    else return false;
    break;
  case 0x20: { const auto a = effective(rsv, ins); schedule_load(rt,
      static_cast<std::uint32_t>(static_cast<std::int32_t>(
          static_cast<std::int8_t>(memory_.iop_read8(a))))); break; }
  case 0x21: { const auto a = effective(rsv, ins); if (a & 1u) { exception = 4; fault_address_ = a; break; }
      schedule_load(rt, static_cast<std::uint32_t>(static_cast<std::int32_t>(
          static_cast<std::int16_t>(memory_.iop_read16(a))))); break; }
  case 0x22: { const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
      const unsigned shift = (a & 3u) * 8u;
      const auto mask = 0x00FFFFFFu >> shift;
      schedule_load(rt, (rtv & mask) | (memory_.iop_read32(aligned) << (24u - shift))); break; }
  case 0x23: { const auto a = effective(rsv, ins); if (a & 3u) { exception = 4; fault_address_ = a; break; }
      schedule_load(rt, memory_.iop_read32(a)); break; }
  case 0x24: { const auto a = effective(rsv, ins); schedule_load(rt, memory_.iop_read8(a)); break; }
  case 0x25: { const auto a = effective(rsv, ins); if (a & 1u) { exception = 4; fault_address_ = a; break; }
      schedule_load(rt, memory_.iop_read16(a)); break; }
  case 0x26: { const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
      const unsigned shift = (a & 3u) * 8u;
      const auto mask = 0xFFFFFF00u << (24u - shift);
      schedule_load(rt, (rtv & mask) | (memory_.iop_read32(aligned) >> shift)); break; }
  case 0x28: { const auto a = effective(rsv, ins); write_memory(a, [&] {
      memory_.iop_write8(a, static_cast<std::uint8_t>(rtv)); }); break; }
  case 0x29: { const auto a = effective(rsv, ins); if (a & 1u) { exception = 5; fault_address_ = a; break; }
      write_memory(a, [&] { memory_.iop_write16(a, static_cast<std::uint16_t>(rtv)); }); break; }
  case 0x2A: { const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
      const unsigned shift = (a & 3u) * 8u;
      const auto replace = 0xFFFFFF00u << shift;
      write_memory(aligned, [&] { memory_.iop_write32(aligned,
          (memory_.iop_read32(aligned) & ~replace) |
          (rtv >> (24u - shift))); }); break; }
  case 0x2B: { const auto a = effective(rsv, ins); if (a & 3u) { exception = 5; fault_address_ = a; break; }
      write_memory(a, [&] { memory_.iop_write32(a, rtv); }); break; }
  case 0x2E: { const auto a = effective(rsv, ins); const auto aligned = a & ~3u;
      const unsigned shift = (a & 3u) * 8u;
      const auto replace = 0x00FFFFFFu >> (24u - shift);
      write_memory(aligned, [&] { memory_.iop_write32(aligned,
          (memory_.iop_read32(aligned) & ~replace) |
          (rtv << shift)); }); break; }
  default: return false;
  }
  return true;
}

const char* iop_stop_reason_name(IopStopReason reason) {
  switch (reason) {
  case IopStopReason::None: return "running";
  case IopStopReason::InvalidInstruction: return "invalid instruction";
  case IopStopReason::MemoryFault: return "memory fault";
  }
  return "unknown";
}

} // namespace ps2vita
