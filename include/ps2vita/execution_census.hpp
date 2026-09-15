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

struct CensusMmioRead {
  std::uint32_t site = 0;
  std::uint32_t address = 0;
  std::uint32_t width = 0;
  std::uint64_t reads = 0;
};

struct CensusEvent {
  std::uint32_t kind = 0;
  std::uint64_t count = 0;
  std::uint64_t min_gap = 0;
  std::uint64_t max_gap = 0;
  std::uint64_t total_gap = 0;
};

// Reconstructs dynamic basic-block entries and edges from an executed MIPS PC
// stream. record() must only be called for instructions that actually retire.
class ExecutionCensus {
public:
  void record(std::uint32_t pc, std::uint32_t instruction,
              std::uint32_t sp = 0, std::uint32_t gp = 0,
              std::uint32_t branch_register_value = 0);
  void record_mmio_read(std::uint32_t site, std::uint32_t address,
                        std::uint32_t width);
  void clear();

  std::uint64_t instruction_count() const { return instruction_count_; }
  std::vector<CensusBlock> blocks() const;
  std::vector<CensusEdge> edges() const;
  std::vector<CensusIndirectTarget> indirect_targets() const;
  std::vector<CensusMmioRead> mmio_reads() const;

private:
  struct BlockStats {
    std::uint64_t entries = 0;
    std::uint32_t sp_min = 0;
    std::uint32_t sp_max = 0;
    std::uint32_t gp_min = 0;
    std::uint32_t gp_max = 0;
  };

  struct MmioReadStats {
    std::uint32_t width = 0;
    std::uint64_t reads = 0;
  };

  static bool has_delay_slot(std::uint32_t instruction);
  static bool is_indirect_branch(std::uint32_t instruction);

  std::unordered_map<std::uint32_t, BlockStats> block_stats_;
  std::unordered_map<std::uint64_t, std::uint64_t> edge_transitions_;
  std::unordered_map<std::uint64_t, std::uint64_t> indirect_transitions_;
  std::unordered_map<std::uint64_t, MmioReadStats> mmio_reads_;
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

class EventCensus {
public:
  void record(std::uint32_t kind, std::uint64_t step);
  void clear();
  std::vector<CensusEvent> events() const;

private:
  struct EventStats {
    std::uint64_t count = 0;
    std::uint64_t first_step = 0;
    std::uint64_t last_step = 0;
    std::uint64_t min_gap = 0;
    std::uint64_t max_gap = 0;
    std::uint64_t total_gap = 0;
  };
  std::unordered_map<std::uint32_t, EventStats> event_stats_;
};

} // namespace ps2vita
