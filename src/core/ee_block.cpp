#include "ps2vita/ee_block.hpp"

#include "ps2vita/memory.hpp"

namespace ps2vita {
namespace {

std::uint8_t classify(std::uint32_t instruction) {
  const unsigned op = instruction >> 26;
  const unsigned rs = (instruction >> 21) & 31u;
  const unsigned fn = instruction & 63u;
  std::uint8_t flags = EeNone;

  if ((op >= 0x20 && op <= 0x3F) || op == 0x1A || op == 0x1B ||
      op == 0x1E || op == 0x1F)
    flags |= EeMemory;
  if (op >= 0x10 && op <= 0x12) flags |= EeCoprocessor;

  const bool special_branch = op == 0 && (fn == 0x08 || fn == 0x09);
  const bool immediate_branch = op == 0x01 || (op >= 0x04 && op <= 0x07) ||
                                (op >= 0x14 && op <= 0x17);
  const bool cop_branch = (op == 0x10 || op == 0x11 || op == 0x12) && rs == 0x08;
  if (special_branch || immediate_branch || op == 0x02 || op == 0x03 || cop_branch)
    flags |= EeBranch;

  const bool special_stop = op == 0 && (fn == 0x0C || fn == 0x0D);
  if (special_stop || instruction == 0x42000018u) flags |= EeStop;
  return flags;
}

} // namespace

EeDecodedBlock EeBlockCache::decode(const Memory& memory, std::uint32_t pc) {
  EeDecodedBlock block;
  block.start_pc = pc;
  block.source_generation = memory.page_generation(pc);
  if ((pc & 3u) != 0 || !memory.valid(pc, 4)) return block;

  bool delay_slot_next = false;
  for (std::size_t i = 0; i < EeDecodedBlock::kMaxInstructions; ++i) {
    const std::uint32_t current = pc + static_cast<std::uint32_t>(i * 4);
    if ((current >> 12) != (pc >> 12) || !memory.valid(current, 4)) break;
    auto& decoded = block.instructions[i];
    decoded.pc = current;
    decoded.opcode = memory.read32(current);
    decoded.flags = classify(decoded.opcode);
    if (delay_slot_next) decoded.flags |= EeDelaySlot;
    block.instruction_count = static_cast<std::uint8_t>(i + 1);

    if (delay_slot_next || (decoded.flags & EeStop)) break;
    delay_slot_next = (decoded.flags & EeBranch) != 0;
  }
  block.valid = block.instruction_count != 0;
  return block;
}

const EeDecodedBlock& EeBlockCache::lookup(const Memory& memory, std::uint32_t pc) {
  auto& slot = slots_[(pc >> 2) & (kSlotCount - 1)];
  const auto generation = memory.page_generation(pc);
  if (!slot.valid || slot.start_pc != pc || slot.source_generation != generation)
    slot = decode(memory, pc);
  if (slot.valid) ++slot.executions;
  return slot;
}

void EeBlockCache::clear() { slots_ = {}; }

std::size_t EeBlockCache::resident_blocks() const {
  std::size_t count = 0;
  for (const auto& slot : slots_) count += slot.valid ? 1u : 0u;
  return count;
}

} // namespace ps2vita
