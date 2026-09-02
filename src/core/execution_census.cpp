#include "ps2vita/execution_census.hpp"

#include <algorithm>

namespace ps2vita {

bool ExecutionCensus::has_delay_slot(std::uint32_t instruction) {
  const auto primary = instruction >> 26;
  if (primary == 0u) {
    const auto function = instruction & 0x3Fu;
    return function == 8u || function == 9u; // JR/JALR
  }
  if (primary == 1u) { // REGIMM branches, including link/likely forms.
    const auto selector = (instruction >> 16) & 0x1Fu;
    return selector <= 3u || (selector >= 16u && selector <= 19u);
  }
  if ((primary >= 2u && primary <= 7u) ||
      (primary >= 0x14u && primary <= 0x17u))
    return true;
  // BCzF/BCzT and their likely forms use rs=8 in COP0/1/2 encodings.
  return primary >= 0x10u && primary <= 0x12u &&
      ((instruction >> 21) & 0x1Fu) == 8u;
}

void ExecutionCensus::record(std::uint32_t pc, std::uint32_t instruction) {
  const bool sequential = active_ && pc == previous_pc_ + 4u;
  const bool executing_delay_slot = pending_delay_slot_ && sequential;
  const bool starts_block = !active_ || start_block_on_next_instruction_ ||
      !sequential;

  if (starts_block) {
    if (active_) {
      const auto key = (static_cast<std::uint64_t>(current_block_) << 32) | pc;
      ++edge_transitions_[key];
    }
    ++block_entries_[pc];
    current_block_ = pc;
    start_block_on_next_instruction_ = false;
    if (!sequential) pending_delay_slot_ = false;
  }

  ++instruction_count_;
  previous_pc_ = pc;
  active_ = true;

  if (executing_delay_slot) {
    pending_delay_slot_ = false;
    start_block_on_next_instruction_ = true;
  } else if (has_delay_slot(instruction)) {
    pending_delay_slot_ = true;
  }
}

void ExecutionCensus::clear() {
  block_entries_.clear();
  edge_transitions_.clear();
  instruction_count_ = 0;
  current_block_ = 0;
  previous_pc_ = 0;
  active_ = false;
  pending_delay_slot_ = false;
  start_block_on_next_instruction_ = false;
}

std::vector<CensusBlock> ExecutionCensus::blocks() const {
  std::vector<CensusBlock> result;
  result.reserve(block_entries_.size());
  for (const auto& item : block_entries_)
    result.push_back({item.first, item.second});
  std::sort(result.begin(), result.end(), [](const auto& left,
                                              const auto& right) {
    return left.pc < right.pc;
  });
  return result;
}

std::vector<CensusEdge> ExecutionCensus::edges() const {
  std::vector<CensusEdge> result;
  result.reserve(edge_transitions_.size());
  for (const auto& item : edge_transitions_) {
    result.push_back({static_cast<std::uint32_t>(item.first >> 32),
                      static_cast<std::uint32_t>(item.first), item.second});
  }
  std::sort(result.begin(), result.end(), [](const auto& left,
                                              const auto& right) {
    return left.source != right.source ? left.source < right.source
                                       : left.target < right.target;
  });
  return result;
}

} // namespace ps2vita
