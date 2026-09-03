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
  std::uint16_t pc = 0;
};

// Functional VU1 micro-mode correctness oracle. Timing and pipeline hazards are
// added as guest software exposes them; instruction pairs remain explicit.
class Vu1 {
public:
  explicit Vu1(Memory& memory) : memory_(memory) { reset(); }
  void reset();
  void start(std::uint16_t address);
  void resume();
  void run(std::uint64_t max_pairs);
  bool pop_path1_packet(std::vector<std::uint8_t>& packet);

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

private:
  bool step();
  bool execute_lower(std::uint32_t code);
  bool execute_upper(std::uint32_t code);
  bool kick_gif(unsigned address_reg);

  Memory& memory_;
  Vu1State state_{};
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
  std::deque<std::vector<std::uint8_t>> path1_packets_;
};

} // namespace ps2vita
