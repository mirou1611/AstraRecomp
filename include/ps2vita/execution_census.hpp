#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace ps2vita {

struct CensusBlock {
  std::uint32_t pc = 0;
  std::uint64_t entries = 0;
};

struct CensusEdge {
  std::uint32_t source = 0;
  std::uint32_t target = 0;
  std::uint64_t transitions = 0;
};

// Reconstructs dynamic basic-block entries and edges from an executed MIPS PC
// stream. record() must only be called for instructions that actually retire.
class ExecutionCensus {
public:
  void record(std::uint32_t pc, std::uint32_t instruction);
  void clear();

  std::uint64_t instruction_count() const { return instruction_count_; }
  std::vector<CensusBlock> blocks() const;
  std::vector<CensusEdge> edges() const;

private:
  static bool has_delay_slot(std::uint32_t instruction);

  std::unordered_map<std::uint32_t, std::uint64_t> block_entries_;
  std::unordered_map<std::uint64_t, std::uint64_t> edge_transitions_;
  std::uint64_t instruction_count_ = 0;
  std::uint32_t current_block_ = 0;
  std::uint32_t previous_pc_ = 0;
  bool active_ = false;
  bool pending_delay_slot_ = false;
  bool start_block_on_next_instruction_ = false;
};

} // namespace ps2vita
