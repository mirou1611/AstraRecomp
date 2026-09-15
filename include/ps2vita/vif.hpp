#pragma once

#include "ps2vita/memory.hpp"
#include "ps2vita/vu.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <vector>

namespace ps2vita {

struct VifUnpackRecord {
  std::uint64_t packet = 0, pair = 0;
  std::size_t source_offset = 0;
  std::uint16_t address = 0;
  std::uint32_t value = 0;
};
struct VifRunRecord {
  std::uint64_t packet = 0, first_pair = 0, end_pair = 0;
  std::size_t command_offset = 0;
  std::uint32_t command = 0;
  std::uint16_t start_pc = 0, end_pc = 0, top = 0;
  std::uint64_t rejected_before = 0, rejected_after = 0;
};

// Minimal VIF1 command frontend. DMA chain transport remains in Memory; this
// class consumes the resulting word stream and updates VU1-visible state.
class Vif1 {
public:
  explicit Vif1(Memory& memory) : memory_(memory), vu1_(memory) {}
  void reset();
  void enable_packet_capture(bool enabled) { capture_packet_ = enabled; }
  void enable_provenance_trace(bool enabled) { trace_provenance_ = enabled; }
  const std::vector<VifUnpackRecord>& unpack_records() const { return unpack_records_; }
  const std::vector<VifRunRecord>& run_records() const { return run_records_; }
  std::uint64_t dropped_unpack_records() const { return dropped_unpack_records_; }
  std::uint64_t dropped_run_records() const { return dropped_run_records_; }
  const std::vector<std::uint8_t>& captured_packet() const { return captured_packet_; }
  bool packet_capture_overflow() const { return capture_overflow_; }
  bool submit(const std::uint8_t* data, std::size_t size);
  bool pop_gif_packet(std::vector<std::uint8_t>& packet);
  std::size_t pending_direct_bytes() const { return direct_remaining_; }

  std::uint64_t packets_submitted() const { return packets_submitted_; }
  std::uint64_t packets_rejected() const { return packets_rejected_; }
  std::uint64_t micro_instructions_loaded() const {
    return micro_instructions_loaded_;
  }
  std::uint64_t vectors_unpacked() const { return vectors_unpacked_; }
  std::uint32_t first_unsupported_code() const {
    return first_unsupported_code_;
  }
  std::uint64_t first_unsupported_packet() const { return first_unsupported_packet_; }
  std::size_t first_unsupported_offset() const { return first_unsupported_offset_; }
  std::size_t first_unsupported_size() const { return first_unsupported_size_; }
  std::uint16_t cycle() const { return cycle_; }
  std::uint16_t top() const { return top_; }
  Vu1& vu1() { return vu1_; }
  const Vu1& vu1() const { return vu1_; }

private:
  void run_vu(std::uint32_t command, std::size_t command_offset);
  Memory& memory_;
  Vu1 vu1_;
  std::uint64_t packets_submitted_ = 0;
  std::uint64_t packets_rejected_ = 0;
  std::uint64_t micro_instructions_loaded_ = 0;
  std::uint64_t vectors_unpacked_ = 0;
  std::uint32_t first_unsupported_code_ = 0;
  std::uint64_t first_unsupported_packet_ = 0;
  std::size_t first_unsupported_offset_ = 0;
  std::size_t first_unsupported_size_ = 0;
  std::uint16_t cycle_ = 0;
  std::uint16_t base_ = 0;
  std::uint16_t offset_ = 0;
  std::uint16_t tops_ = 0;
  std::uint16_t itops_ = 0;
  std::uint16_t top_ = 0;
  bool double_buffer_ = false;
  bool capture_packet_ = false;
  bool capture_overflow_ = false;
  std::vector<std::uint8_t> captured_packet_;
  std::size_t direct_remaining_ = 0;
  std::vector<std::uint8_t> direct_packet_;
  std::deque<std::vector<std::uint8_t>> gif_packets_;
  bool trace_provenance_ = false;
  std::vector<VifUnpackRecord> unpack_records_;
  std::vector<VifRunRecord> run_records_;
  std::uint64_t dropped_unpack_records_ = 0, dropped_run_records_ = 0;
};

} // namespace ps2vita
