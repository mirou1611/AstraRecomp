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

bool ExecutionCensus::is_indirect_branch(std::uint32_t instruction) {
  if ((instruction >> 26) != 0u) return false;
  const auto function = instruction & 0x3Fu;
  return function == 8u || function == 9u;
}

void ExecutionCensus::record(std::uint32_t pc, std::uint32_t instruction,
                             std::uint32_t sp, std::uint32_t gp,
                             std::uint32_t branch_register_value) {
  const bool sequential = active_ && pc == previous_pc_ + 4u;
  const bool executing_delay_slot = pending_delay_slot_ && sequential;
  const bool starts_block = !active_ || start_block_on_next_instruction_ ||
      !sequential;

  if (starts_block) {
    if (active_) {
      const auto key = (static_cast<std::uint64_t>(current_block_) << 32) | pc;
      ++edge_transitions_[key];
    }
    if (indirect_on_next_instruction_ &&
        pc == indirect_target_on_next_instruction_) {
      const auto key =
          (static_cast<std::uint64_t>(indirect_site_on_next_instruction_) << 32) |
          pc;
      ++indirect_transitions_[key];
    }
    auto& stats = block_stats_[pc];
    if (stats.entries++ == 0u) {
      stats.sp_min = stats.sp_max = sp;
      stats.gp_min = stats.gp_max = gp;
    } else {
      stats.sp_min = std::min(stats.sp_min, sp);
      stats.sp_max = std::max(stats.sp_max, sp);
      stats.gp_min = std::min(stats.gp_min, gp);
      stats.gp_max = std::max(stats.gp_max, gp);
    }
    current_block_ = pc;
    start_block_on_next_instruction_ = false;
    indirect_on_next_instruction_ = false;
    if (!sequential) {
      pending_delay_slot_ = false;
      pending_indirect_ = false;
    }
  }

  ++instruction_count_;
  previous_pc_ = pc;
  active_ = true;

  if (executing_delay_slot) {
    pending_delay_slot_ = false;
    start_block_on_next_instruction_ = true;
    indirect_on_next_instruction_ = pending_indirect_;
    indirect_site_on_next_instruction_ = pending_indirect_site_;
    indirect_target_on_next_instruction_ = pending_indirect_target_;
    pending_indirect_ = false;
  } else if (has_delay_slot(instruction)) {
    pending_delay_slot_ = true;
    pending_indirect_ = is_indirect_branch(instruction);
    pending_indirect_site_ = pc;
    pending_indirect_target_ = branch_register_value;
  }
}

void ExecutionCensus::record_mmio_read(std::uint32_t site,
                                       std::uint32_t address,
                                       std::uint32_t width) {
  const auto key = (static_cast<std::uint64_t>(site) << 32) | address;
  auto& stats = mmio_reads_[key];
  stats.width = width;
  ++stats.reads;
}

void ExecutionCensus::clear() {
  block_stats_.clear();
  edge_transitions_.clear();
  indirect_transitions_.clear();
  mmio_reads_.clear();
  instruction_count_ = 0;
  current_block_ = 0;
  previous_pc_ = 0;
  active_ = false;
  pending_delay_slot_ = false;
  start_block_on_next_instruction_ = false;
  pending_indirect_ = false;
  indirect_on_next_instruction_ = false;
  pending_indirect_site_ = 0;
  pending_indirect_target_ = 0;
  indirect_site_on_next_instruction_ = 0;
  indirect_target_on_next_instruction_ = 0;
}

std::vector<CensusMmioRead> ExecutionCensus::mmio_reads() const {
  std::vector<CensusMmioRead> result;
  result.reserve(mmio_reads_.size());
  for (const auto& item : mmio_reads_) {
    result.push_back({static_cast<std::uint32_t>(item.first >> 32),
                      static_cast<std::uint32_t>(item.first),
                      item.second.width, item.second.reads});
  }
  std::sort(result.begin(), result.end(), [](const auto& left,
                                              const auto& right) {
    return left.site != right.site ? left.site < right.site
                                   : left.address < right.address;
  });
  return result;
}

std::vector<CensusBlock> ExecutionCensus::blocks() const {
  std::vector<CensusBlock> result;
  result.reserve(block_stats_.size());
  for (const auto& item : block_stats_) {
    result.push_back({item.first, item.second.entries,
                      item.second.sp_min, item.second.sp_max,
                      item.second.gp_min, item.second.gp_max});
  }
  std::sort(result.begin(), result.end(), [](const auto& left,
                                              const auto& right) {
    return left.pc < right.pc;
  });
  return result;
}

std::vector<CensusIndirectTarget> ExecutionCensus::indirect_targets() const {
  std::vector<CensusIndirectTarget> result;
  result.reserve(indirect_transitions_.size());
  for (const auto& item : indirect_transitions_) {
    result.push_back({static_cast<std::uint32_t>(item.first >> 32),
                      static_cast<std::uint32_t>(item.first), item.second});
  }
  std::sort(result.begin(), result.end(), [](const auto& left,
                                              const auto& right) {
    return left.site != right.site ? left.site < right.site
                                   : left.target < right.target;
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

void EventCensus::record(std::uint32_t kind, std::uint64_t step) {
  auto& stats = event_stats_[kind];
  if (stats.count == 0u) {
    stats.first_step = stats.last_step = step;
    stats.count = 1u;
    return;
  }
  const auto gap = step - stats.last_step;
  if (stats.count == 1u || gap < stats.min_gap) stats.min_gap = gap;
  stats.max_gap = std::max(stats.max_gap, gap);
  stats.total_gap += gap;
  stats.last_step = step;
  ++stats.count;
}

void EventCensus::clear() {
  event_stats_.clear();
}

std::vector<CensusEvent> EventCensus::events() const {
  std::vector<CensusEvent> result;
  result.reserve(event_stats_.size());
  for (const auto& item : event_stats_) {
    result.push_back({item.first, item.second.count, item.second.min_gap,
                      item.second.max_gap, item.second.total_gap});
  }
  std::sort(result.begin(), result.end(), [](const auto& left,
                                              const auto& right) {
    return left.kind < right.kind;
  });
  return result;
}

} // namespace ps2vita
