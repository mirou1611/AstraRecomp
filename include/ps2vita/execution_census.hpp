#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ps2vita {

struct CensusBlock {
  std::uint32_t pc = 0;
  std::uint64_t entries = 0;
  std::uint32_t sp_min = 0;
  std::uint32_t sp_max = 0;
  std::uint32_t gp_min = 0;
  std::uint32_t gp_max = 0;
};

struct CensusEdge {
  std::uint32_t source = 0;
  std::uint32_t target = 0;
  std::uint64_t transitions = 0;
};

struct CensusIndirectTarget {
  std::uint32_t site = 0;
  std::uint32_t target = 0;
  std::uint64_t transitions = 0;
};

// Reconstructs dynamic basic-block entries and edges from an executed MIPS PC
// stream. record() must only be called for instructions that actually retire.
class ExecutionCensus {
public:
  void record(std::uint32_t pc, std::uint32_t instruction,
              std::uint32_t sp = 0, std::uint32_t gp = 0,
              std::uint32_t branch_register_value = 0);
  void clear();

  std::uint64_t instruction_count() const { return instruction_count_; }
  std::vector<CensusBlock> blocks() const;
  std::vector<CensusEdge> edges() const;
  std::vector<CensusIndirectTarget> indirect_targets() const;

private:
  struct BlockStats {
    std::uint64_t entries = 0;
    std::uint32_t sp_min = 0;
    std::uint32_t sp_max = 0;
    std::uint32_t gp_min = 0;
    std::uint32_t gp_max = 0;
  };

  static bool has_delay_slot(std::uint32_t instruction);
  static bool is_indirect_branch(std::uint32_t instruction);

  std::unordered_map<std::uint32_t, BlockStats> block_stats_;
  std::unordered_map<std::uint64_t, std::uint64_t> edge_transitions_;
  std::unordered_map<std::uint64_t, std::uint64_t> indirect_transitions_;
  std::uint64_t instruction_count_ = 0;
  std::uint32_t current_block_ = 0;
  std::uint32_t previous_pc_ = 0;
  bool active_ = false;
  bool pending_delay_slot_ = false;
  bool start_block_on_next_instruction_ = false;
  bool pending_indirect_ = false;
  bool indirect_on_next_instruction_ = false;
  std::uint32_t pending_indirect_site_ = 0;
  std::uint32_t pending_indirect_target_ = 0;
  std::uint32_t indirect_site_on_next_instruction_ = 0;
  std::uint32_t indirect_target_on_next_instruction_ = 0;
};

} // namespace ps2vita
