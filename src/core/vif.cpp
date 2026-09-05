#include "ps2vita/vif.hpp"

#include <cstring>

namespace ps2vita {
namespace {

// Defensive execution ceiling, not a measured BIOS workload. The former
// million-pair loop was caused by incorrect IADDIU sign extension.
constexpr std::uint64_t kVu1ExecutionBudget = 2000000u;

std::uint32_t load32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

} // namespace

void Vif1::reset() {
  vu1_.reset();
  packets_submitted_ = 0;
  packets_rejected_ = 0;
  micro_instructions_loaded_ = 0;
  vectors_unpacked_ = 0;
  first_unsupported_code_ = 0;
  cycle_ = 0;
  base_ = 0;
  offset_ = 0;
  tops_ = 0;
  itops_ = 0;
  top_ = 0;
  double_buffer_ = false;
}

bool Vif1::submit(const std::uint8_t* data, std::size_t size) {
  ++packets_submitted_;
  std::size_t cursor = 0;
  while (cursor + 4u <= size) {
    const auto code = load32(data + cursor);
    cursor += 4u;
    const auto command = static_cast<std::uint8_t>(code >> 24);
    const auto opcode = command & 0x7Fu;
    if (command == 0x00u) continue; // NOP
    if (opcode == 0x01u) { // STCYCL
      cycle_ = static_cast<std::uint16_t>(code);
      continue;
    }
    if (opcode == 0x02u) { // OFFSET
      offset_ = static_cast<std::uint16_t>(code & 0x3FFu);
      tops_ = base_;
      double_buffer_ = false;
      continue;
    }
    if (opcode == 0x03u) { // BASE
      base_ = static_cast<std::uint16_t>(code & 0x3FFu);
      continue;
    }
    if (opcode == 0x04u) { // ITOP
      itops_ = static_cast<std::uint16_t>(code & 0x3FFu);
      continue;
    }
    if ((opcode >= 0x05u && opcode <= 0x07u) || opcode == 0x10u ||
        opcode == 0x11u || opcode == 0x13u) {
      continue; // Register state / synchronization without stream payload.
    }
    if (opcode == 0x20u) { // STMASK
      if (cursor + 4u > size) { ++packets_rejected_; return false; }
      cursor += 4u;
      continue;
    }
    if (opcode == 0x14u || opcode == 0x15u) { // MSCAL / MSCALF
      top_ = tops_ & 0x3FFu;
      tops_ = double_buffer_ ? base_ :
          static_cast<std::uint16_t>((base_ + offset_) & 0x3FFu);
      double_buffer_ = !double_buffer_;
      vu1_.set_top(top_);
      vu1_.start(static_cast<std::uint16_t>((code & 0x3FFu) * 8u));
      vu1_.run(kVu1ExecutionBudget);
      if (vu1_.running()) {
        if (first_unsupported_code_ == 0u) first_unsupported_code_ = code;
        ++packets_rejected_;
        return false;
      }
      continue;
    }
    if (opcode == 0x17u) { // MSCNT: continue at the current VU1 TPC.
      top_ = tops_ & 0x3FFu;
      tops_ = double_buffer_ ? base_ :
          static_cast<std::uint16_t>((base_ + offset_) & 0x3FFu);
      double_buffer_ = !double_buffer_;
      vu1_.set_top(top_);
      vu1_.resume();
      vu1_.run(kVu1ExecutionBudget);
      if (vu1_.running()) {
        if (first_unsupported_code_ == 0u) first_unsupported_code_ = code;
        ++packets_rejected_;
        return false;
      }
      continue;
    }
    if (opcode == 0x30u || opcode == 0x31u) { // STROW / STCOL
      if (cursor + 16u > size) { ++packets_rejected_; return false; }
      cursor += 16u;
      continue;
    }
    if (opcode == 0x4Au) { // MPG: VU1 microprogram upload.
      auto count = static_cast<unsigned>((code >> 16) & 0xFFu);
      if (count == 0u) count = 256u;
      const auto address = static_cast<unsigned>(code & 0x3FFu);
      const auto bytes = static_cast<std::size_t>(count) * 8u;
      if (cursor + bytes > size) { ++packets_rejected_; return false; }
      for (std::size_t byte = 0; byte < bytes; byte += 4u) {
        const auto destination = Memory::kVu1MicroBase +
            static_cast<std::uint32_t>((address * 8u + byte) & 0x3FFFu);
        memory_.write32(destination, load32(data + cursor + byte));
      }
      cursor += bytes;
      micro_instructions_loaded_ += count;
      continue;
    }
    if (opcode == 0x6Cu) { // UNPACK V4-32, contiguous cycle mode.
      auto count = static_cast<unsigned>((code >> 16) & 0xFFu);
      if (count == 0u) count = 256u;
      const auto cl = static_cast<unsigned>(cycle_ & 0xFFu);
      const auto wl = static_cast<unsigned>(cycle_ >> 8);
      if ((cl != 0u || wl != 0u) && cl != wl) {
        if (first_unsupported_code_ == 0u) first_unsupported_code_ = code;
        ++packets_rejected_;
        return false;
      }
      const auto bytes = static_cast<std::size_t>(count) * 16u;
      if (cursor + bytes > size) { ++packets_rejected_; return false; }
      auto qword_address = static_cast<unsigned>(code & 0x3FFu);
      if ((code & 0x8000u) != 0u)
        qword_address = (qword_address + tops_) & 0x3FFu;
      const auto address = qword_address * 16u;
      for (std::size_t byte = 0; byte < bytes; byte += 4u) {
        const auto destination = Memory::kVu1DataBase +
            static_cast<std::uint32_t>((address + byte) & 0x3FFFu);
        memory_.write32(destination, load32(data + cursor + byte));
      }
      cursor += bytes;
      vectors_unpacked_ += count;
      continue;
    }

    if (first_unsupported_code_ == 0u) first_unsupported_code_ = code;
    ++packets_rejected_;
    return false;
  }
  if (cursor != size) { ++packets_rejected_; return false; }
  return true;
}

} // namespace ps2vita
