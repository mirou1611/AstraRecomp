#pragma once

#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2vita {

struct Vu1State {
  std::array<std::array<std::uint32_t, 4>, 32> vf{};
  std::array<std::uint16_t, 16> vi{};
  std::array<std::uint32_t, 4> acc{};
  std::uint32_t i = 0;
  std::uint32_t q = 0;
  std::uint16_t mac = 0;
  std::uint16_t pc = 0;
};
struct Vu1StoreRecord {
  std::uint64_t pair = 0;
  std::uint64_t cycle = 0;
  std::uint16_t pc = 0, address = 0;
  std::uint32_t value = 0;
};

// Functional VU1 micro-mode correctness oracle. Timing and pipeline hazards are
// added as guest software exposes them; instruction pairs remain explicit.
class Vu1 {
public:
  explicit Vu1(Memory& memory) : memory_(memory) { reset(); }
  void reset();
  void start(std::uint16_t address);
  void resume();
  void set_top(std::uint16_t top) { top_ = top & 0x3FFu; }
  void run(std::uint64_t max_pairs);
  bool pop_path1_packet(std::vector<std::uint8_t>& packet);
  void enable_store_trace(bool enabled) { trace_stores_ = enabled; }
  const std::vector<Vu1StoreRecord>& store_records() const { return store_records_; }
  std::uint64_t dropped_store_records() const { return dropped_store_records_; }
  std::uint64_t first_rejected_pair() const { return first_rejected_pair_; }
  std::uint64_t cycles_executed() const { return cycles_; }
  std::uint64_t vf_stall_cycles() const { return vf_stall_cycles_; }
  std::uint64_t q_stall_cycles() const { return q_stall_cycles_; }
  std::uint64_t xgkick_stall_cycles() const { return xgkick_stall_cycles_; }
  bool path1_active() const { return kick_active_; }

  Vu1State& state() { return state_; }
  const Vu1State& state() const { return state_; }
  bool running() const { return running_; }
  std::uint64_t pairs_executed() const { return pairs_executed_; }
  std::uint32_t first_unsupported_lower() const {
    return first_unsupported_lower_;
  }
  std::uint32_t first_unsupported_upper() const {
    return first_unsupported_upper_;
  }
  std::uint16_t last_kick_address() const { return last_kick_address_; }
  std::uint64_t last_kick_tag() const { return last_kick_tag_; }
  std::uint64_t path1_tags_queued() const { return path1_tags_queued_; }
  std::uint64_t path1_tags_rejected() const { return path1_tags_rejected_; }
  std::uint64_t first_rejected_tag() const { return first_rejected_tag_; }
  std::uint16_t first_rejected_address() const { return first_rejected_address_; }
  std::uint16_t first_rejected_pc() const { return first_rejected_pc_; }
  std::uint16_t first_rejected_kick_start() const { return first_rejected_kick_start_; }
  unsigned first_rejected_tag_index() const { return first_rejected_tag_index_; }
  std::uint64_t first_rejected_previous_tag() const { return first_rejected_previous_tag_; }
  const std::array<std::uint32_t, 32>& first_rejected_data() const {
    return first_rejected_data_;
  }

private:
  bool step();
  bool execute_lower(std::uint32_t code);
  bool execute_upper(std::uint32_t code);
  bool kick_gif(unsigned address_reg);
  void transfer_path1(bool flush);
  void store_data(std::uint32_t address, std::uint32_t value);

  Memory& memory_;
  Vu1State state_{};
  std::array<std::array<std::uint64_t, 4>, 32> vf_ready_{};
  std::uint64_t cycles_ = 0, vf_stall_cycles_ = 0;
  std::uint64_t q_ready_ = 0, q_stall_cycles_ = 0;
  std::uint32_t pending_q_ = 0;
  bool q_pending_ = false;
  bool trace_stores_ = false;
  std::vector<Vu1StoreRecord> store_records_;
  std::uint64_t dropped_store_records_ = 0, first_rejected_pair_ = 0;
  std::array<std::array<std::uint32_t, 4>, 32> lower_vf_snapshot_{};
  bool running_ = false;
  bool branch_pending_ = false;
  bool end_pending_ = false;
  std::uint16_t branch_target_ = 0;
  std::uint64_t pairs_executed_ = 0;
  std::uint32_t first_unsupported_lower_ = 0;
  std::uint32_t first_unsupported_upper_ = 0;
  std::uint16_t last_kick_address_ = 0;
  std::uint64_t last_kick_tag_ = 0;
  std::uint64_t path1_tags_queued_ = 0;
  std::uint64_t path1_tags_rejected_ = 0;
  std::uint64_t first_rejected_tag_ = 0;
  std::uint16_t first_rejected_address_ = 0;
  std::uint16_t first_rejected_pc_ = 0;
  std::uint16_t first_rejected_kick_start_ = 0;
  unsigned first_rejected_tag_index_ = 0;
  std::uint64_t first_rejected_previous_tag_ = 0;
  std::array<std::uint32_t, 32> first_rejected_data_{};
  std::uint16_t top_ = 0;
  std::uint16_t lower_mac_snapshot_ = 0;
  // Four cycle slots for MAC visibility, advanced during modeled VF stalls.
  // Other pipelines and external synchronization are not a complete cycle model.
  std::array<std::uint16_t, 4> mac_pipeline_{};
  unsigned mac_pipeline_slot_ = 0;
  std::deque<std::vector<std::uint8_t>> path1_packets_;
  bool kick_active_ = false, kick_eop_ = false;
  std::uint16_t kick_offset_ = 0, kick_pc_ = 0;
  unsigned kick_tag_index_ = 0, kick_remaining_ = 0;
  std::uint64_t kick_next_cycle_ = 0, kick_previous_tag_ = 0;
  std::uint64_t kick_tag_ = 0, xgkick_stall_cycles_ = 0;
  std::vector<std::uint8_t> kick_packet_;
};

} // namespace ps2vita
