#pragma once

#include "ps2vita/memory.hpp"
#include "ps2vita/vu.hpp"

#include <cstddef>
#include <cstdint>

namespace ps2vita {

// Minimal VIF1 command frontend. DMA chain transport remains in Memory; this
// class consumes the resulting word stream and updates VU1-visible state.
class Vif1 {
public:
  explicit Vif1(Memory& memory) : memory_(memory), vu1_(memory) {}
  void reset();
  bool submit(const std::uint8_t* data, std::size_t size);

  std::uint64_t packets_submitted() const { return packets_submitted_; }
  std::uint64_t packets_rejected() const { return packets_rejected_; }
  std::uint64_t micro_instructions_loaded() const {
    return micro_instructions_loaded_;
  }
  std::uint64_t vectors_unpacked() const { return vectors_unpacked_; }
  std::uint32_t first_unsupported_code() const {
    return first_unsupported_code_;
  }
  std::uint16_t cycle() const { return cycle_; }
  Vu1& vu1() { return vu1_; }
  const Vu1& vu1() const { return vu1_; }

private:
  Memory& memory_;
  Vu1 vu1_;
  std::uint64_t packets_submitted_ = 0;
  std::uint64_t packets_rejected_ = 0;
  std::uint64_t micro_instructions_loaded_ = 0;
  std::uint64_t vectors_unpacked_ = 0;
  std::uint32_t first_unsupported_code_ = 0;
  std::uint16_t cycle_ = 0;
};

} // namespace ps2vita
