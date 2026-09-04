#include "ps2vita/vu.hpp"

#include <cmath>
#include <cstring>
#include <limits>

namespace ps2vita {
namespace {

std::int32_t sign_extend(std::uint32_t value, unsigned bits) {
  const auto shift = 32u - bits;
  return static_cast<std::int32_t>(value << shift) >> shift;
}

float as_float(std::uint32_t bits) {
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

std::uint32_t as_bits(float value) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  return bits;
}

std::uint32_t update_mac(std::uint16_t& mac, unsigned lane,
                         std::uint32_t bits) {
  const auto shift = 3u - lane;
  const auto lane_mask = static_cast<std::uint16_t>(0x1111u << shift);
  mac &= static_cast<std::uint16_t>(~lane_mask);
  const auto sign = bits & 0x80000000u;
  if (sign != 0u) mac |= static_cast<std::uint16_t>(0x10u << shift);
  const auto exponent = (bits >> 23) & 0xFFu;
  if ((bits & 0x7FFFFFFFu) == 0u) {
    mac |= static_cast<std::uint16_t>(1u << shift);
  } else if (exponent == 0u) {
    mac |= static_cast<std::uint16_t>(0x101u << shift);
    return sign;
  } else if (exponent == 0xFFu) {
    mac |= static_cast<std::uint16_t>(0x1000u << shift);
    return sign | 0x7F7FFFFFu;
  }
  return bits;
}

std::uint32_t float_to_int(std::uint32_t bits, unsigned scale) {
  const auto value = std::ldexp(static_cast<double>(as_float(bits)), scale);
  if (std::isnan(value))
    return (bits & 0x80000000u) != 0u ? 0x80000000u : 0x7FFFFFFFu;
  if (value >= static_cast<double>(std::numeric_limits<std::int32_t>::max()))
    return 0x7FFFFFFFu;
  if (value <= static_cast<double>(std::numeric_limits<std::int32_t>::min()))
    return 0x80000000u;
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(value));
}

} // namespace

void Vu1::reset() {
  state_ = {};
  state_.vf[0][3] = 0x3F800000u;
  running_ = false;
  branch_pending_ = false;
  end_pending_ = false;
  branch_target_ = 0;
  pairs_executed_ = 0;
  first_unsupported_lower_ = 0;
  first_unsupported_upper_ = 0;
  last_kick_address_ = 0;
  last_kick_tag_ = 0;
  path1_tags_queued_ = 0;
  path1_tags_rejected_ = 0;
  top_ = 0;
  path1_packets_.clear();
}

void Vu1::start(std::uint16_t address) {
  state_.pc = address & 0x3FF8u;
  running_ = true;
  branch_pending_ = false;
  end_pending_ = false;
}

void Vu1::resume() {
  running_ = true;
  branch_pending_ = false;
  end_pending_ = false;
}

void Vu1::run(std::uint64_t max_pairs) {
  while (running_ && max_pairs-- != 0u && step()) {}
}

bool Vu1::step() {
  const auto address = Memory::kVu1MicroBase + state_.pc;
  const auto lower = memory_.read32(address);
  const auto upper = memory_.read32(address + 4u);
  const auto sequential_pc = static_cast<std::uint16_t>((state_.pc + 8u) & 0x3FFFu);
  const bool apply_branch = branch_pending_;
  const auto pending_target = branch_target_;
  const bool apply_end = end_pending_;
  lower_mac_snapshot_ = state_.mac;
  branch_pending_ = false;
  end_pending_ = (upper & 0x40000000u) != 0u;

  if (!execute_upper(upper)) {
    first_unsupported_upper_ = upper;
    running_ = false;
    return false;
  }
  // When I is set, the lower word is the immediate register payload.
  if ((upper & 0x80000000u) != 0u) state_.i = lower;
  else if (!execute_lower(lower)) {
    first_unsupported_lower_ = lower;
    running_ = false;
    return false;
  }

  ++pairs_executed_;
  state_.pc = apply_branch ? pending_target : sequential_pc;
  if (apply_end) running_ = false;
  return true;
}

bool Vu1::execute_lower(std::uint32_t code) {
  const auto group = code >> 25;
  const auto it = static_cast<unsigned>((code >> 16) & 0xFu);
  const auto is = static_cast<unsigned>((code >> 11) & 0xFu);
  if (group == 0x00u || group == 0x01u) { // LQ / SQ
    const bool store = group == 0x01u;
    const auto vector_reg = static_cast<unsigned>(
        store ? (code >> 11) & 0x1Fu : (code >> 16) & 0x1Fu);
    const auto address_reg = store ? it : is;
    const auto qword = static_cast<std::uint32_t>(
        state_.vi[address_reg] + sign_extend(code & 0x7FFu, 11u)) & 0x3FFu;
    for (unsigned lane = 0; lane < 4u; ++lane) {
      if ((code & (1u << (24u - lane))) == 0u) continue;
      const auto address = Memory::kVu1DataBase + qword * 16u + lane * 4u;
      if (store) memory_.write32(address, state_.vf[vector_reg][lane]);
      else if (vector_reg != 0u) state_.vf[vector_reg][lane] = memory_.read32(address);
    }
    return true;
  }
  if (group == 0x1Au) { // FMAND
    if (it != 0u) state_.vi[it] = static_cast<std::uint16_t>(
        lower_mac_snapshot_ & state_.vi[is]);
    return true;
  }
  if (group == 0x08u || group == 0x09u) { // IADDIU / ISUBIU
    const auto raw = ((code >> 10) & 0x7800u) | (code & 0x7FFu);
    const auto immediate = sign_extend(raw, 15u);
    const auto lhs = static_cast<std::int32_t>(state_.vi[is]);
    const auto result = group == 0x08u ? lhs + immediate : lhs - immediate;
    if (it != 0u) state_.vi[it] = static_cast<std::uint16_t>(result);
    return true;
  }
  if (group == 0x21u) { // BAL
    if (it != 0u) state_.vi[it] = static_cast<std::uint16_t>(
        ((state_.pc + 16u) & 0x3FFFu) >> 3);
    branch_target_ = static_cast<std::uint16_t>((state_.pc + 8u +
        sign_extend(code & 0x7FFu, 11u) * 8) & 0x3FFFu);
    branch_pending_ = true;
    return true;
  }
  if (group == 0x20u) { // B
    branch_target_ = static_cast<std::uint16_t>((state_.pc + 8u +
        sign_extend(code & 0x7FFu, 11u) * 8) & 0x3FFFu);
    branch_pending_ = true;
    return true;
  }
  if (group == 0x24u) { // JR
    branch_target_ = static_cast<std::uint16_t>((state_.vi[is] * 8u) & 0x3FFFu);
    branch_pending_ = true;
    return true;
  }
  if (group == 0x28u || group == 0x29u) { // IBEQ / IBNE
    const bool equal = state_.vi[is] == state_.vi[it];
    if ((group == 0x28u && equal) || (group == 0x29u && !equal)) {
      branch_target_ = static_cast<std::uint16_t>((state_.pc + 8u +
          sign_extend(code & 0x7FFu, 11u) * 8) & 0x3FFFu);
      branch_pending_ = true;
    }
    return true;
  }
  if (group == 0x2Eu) { // IBLEZ
    if (static_cast<std::int16_t>(state_.vi[is]) <= 0) {
      branch_target_ = static_cast<std::uint16_t>((state_.pc + 8u +
          sign_extend(code & 0x7FFu, 11u) * 8) & 0x3FFFu);
      branch_pending_ = true;
    }
    return true;
  }
  if (group != 0x40u) return false;

  const auto function = static_cast<unsigned>(code & 0x3Fu);
  const auto fd = static_cast<unsigned>((code >> 6) & 0x1Fu);
  if (function == 0x30u) { // IADD
    const auto id = fd & 0xFu;
    if (id != 0u) state_.vi[id] = static_cast<std::uint16_t>(
        state_.vi[is] + state_.vi[it]);
    return true;
  }
  if (function == 0x32u) { // IADDI
    const auto immediate = sign_extend((code >> 6) & 0x1Fu, 5u);
    if (it != 0u) state_.vi[it] = static_cast<std::uint16_t>(
        static_cast<std::int32_t>(state_.vi[is]) + immediate);
    return true;
  }
  if (function == 0x34u || function == 0x35u) { // IAND / IOR
    const auto id = fd & 0xFu;
    if (id != 0u) state_.vi[id] = static_cast<std::uint16_t>(
        function == 0x34u ? state_.vi[is] & state_.vi[it]
                          : state_.vi[is] | state_.vi[it]);
    return true;
  }
  if (function == 0x3Cu && fd == 0x0Cu) { // MOVE encoding of lower NOP.
    const auto fs = static_cast<unsigned>((code >> 11) & 0x1Fu);
    const auto ft = static_cast<unsigned>((code >> 16) & 0x1Fu);
    return fs == 0u && ft == 0u;
  }
  if (function == 0x3Cu && fd == 0x0Du) { // LQI
    const auto ft = static_cast<unsigned>((code >> 16) & 0x1Fu);
    const auto qword = state_.vi[is] & 0x3FFu;
    for (unsigned lane = 0; lane < 4u; ++lane) {
      const auto mask = 1u << (24u - lane);
      if ((code & mask) != 0u && ft != 0u)
        state_.vf[ft][lane] = memory_.read32(
            Memory::kVu1DataBase + qword * 16u + lane * 4u);
    }
    if (is != 0u) ++state_.vi[is];
    return true;
  }
  if (function == 0x3Cu && fd == 0x0Eu) { // DIV
    const auto fs = static_cast<unsigned>((code >> 11) & 0x1Fu);
    const auto ft = static_cast<unsigned>((code >> 16) & 0x1Fu);
    state_.q = as_bits(as_float(state_.vf[fs][(code >> 21) & 3u]) /
                       as_float(state_.vf[ft][(code >> 23) & 3u]));
    return true;
  }
  if (function == 0x3Cu && fd == 0x0Fu) { // MTIR
    if (it != 0u) state_.vi[it] = static_cast<std::uint16_t>(
        state_.vf[(code >> 11) & 0x1Fu][(code >> 21) & 3u]);
    return true;
  }
  if (function == 0x3Cu && fd == 0x1Au) { // XTOP
    if (it != 0u) state_.vi[it] = top_;
    return true;
  }
  if (function == 0x3Cu && fd == 0x1Bu) // XGKICK
    return kick_gif(is);
  if (function == 0x3Du && fd == 0x0Du) { // SQI
    const auto fs = static_cast<unsigned>((code >> 11) & 0x1Fu);
    const auto address_reg = static_cast<unsigned>((code >> 16) & 0xFu);
    const auto qword = state_.vi[address_reg] & 0x3FFu;
    for (unsigned lane = 0; lane < 4u; ++lane) {
      const auto mask = 1u << (24u - lane);
      if ((code & mask) != 0u)
        memory_.write32(Memory::kVu1DataBase + qword * 16u + lane * 4u,
                        state_.vf[fs][lane]);
    }
    if (address_reg != 0u) ++state_.vi[address_reg];
    return true;
  }
  if (function == 0x3Du && fd == 0x0Fu) { // MFIR
    const auto ft = static_cast<unsigned>((code >> 16) & 0x1Fu);
    const auto value = static_cast<std::uint32_t>(static_cast<std::int32_t>(
        static_cast<std::int16_t>(state_.vi[is])));
    if (ft != 0u) {
      for (unsigned lane = 0; lane < 4u; ++lane) {
        if ((code & (1u << (24u - lane))) != 0u) state_.vf[ft][lane] = value;
      }
    }
    return true;
  }
  if (function == 0x3Eu && fd == 0x0Fu) { // ILWR
    if (it != 0u) {
      const auto qword = state_.vi[is] & 0x3FFu;
      for (unsigned lane = 0; lane < 4u; ++lane) {
        if ((code & (1u << (24u - lane))) != 0u)
          state_.vi[it] = static_cast<std::uint16_t>(memory_.read32(
              Memory::kVu1DataBase + qword * 16u + lane * 4u));
      }
    }
    return true;
  }
  if (function == 0x3Fu && fd == 0x0Eu) return true; // WAITQ
  return false;
}

bool Vu1::kick_gif(unsigned address_reg) {
  auto offset = static_cast<std::uint32_t>(state_.vi[address_reg] & 0x3FFu) * 16u;
  last_kick_address_ = static_cast<std::uint16_t>(offset);
  last_kick_tag_ = 0;
  for (unsigned tag_index = 0; tag_index < 256u; ++tag_index) {
    std::array<std::uint8_t, 16> tag_bytes{};
    for (unsigned byte = 0; byte < tag_bytes.size(); ++byte)
      tag_bytes[byte] = memory_.read8(
          Memory::kVu1DataBase + ((offset + byte) & 0x3FFFu));
    std::uint64_t tag = 0;
    std::memcpy(&tag, tag_bytes.data(), sizeof(tag));
    if (tag_index == 0u) last_kick_tag_ = tag;
    std::vector<std::uint8_t> packet(tag_bytes.begin(), tag_bytes.end());
    offset = (offset + 16u) & 0x3FFFu;

    const auto loops = static_cast<std::uint32_t>(tag & 0x7FFFu);
    const auto format = static_cast<unsigned>((tag >> 58) & 3u);
    auto registers = static_cast<std::uint32_t>((tag >> 60) & 0xFu);
    if (registers == 0u) registers = 16u;
    std::uint64_t payload_size = 0;
    if (format == 0u) payload_size = std::uint64_t{loops} * registers * 16u;
    else if (format == 1u)
      payload_size = ((std::uint64_t{loops} * registers + 1u) / 2u) * 16u;
    else payload_size = std::uint64_t{loops} * 16u;
    if (payload_size > 0x3FF0u) {
      ++path1_tags_rejected_;
      return true;
    }
    for (std::uint64_t byte = 0; byte < payload_size; ++byte)
      packet.push_back(memory_.read8(Memory::kVu1DataBase +
          ((offset + static_cast<std::uint32_t>(byte)) & 0x3FFFu)));
    offset = (offset + static_cast<std::uint32_t>(payload_size)) & 0x3FFFu;
    path1_packets_.push_back(std::move(packet));
    ++path1_tags_queued_;
    if ((tag & (1ull << 15)) != 0u) return true;
  }
  ++path1_tags_rejected_;
  return true;
}

bool Vu1::pop_path1_packet(std::vector<std::uint8_t>& packet) {
  if (path1_packets_.empty()) return false;
  packet = std::move(path1_packets_.front());
  path1_packets_.pop_front();
  return true;
}

bool Vu1::execute_upper(std::uint32_t code) {
  const auto function = static_cast<unsigned>(code & 0x3Fu);
  const auto fd = static_cast<unsigned>((code >> 6) & 0x1Fu);
  // Upper NOP is the FD=11 member of the 0x3F special table. Control flags in
  // the high bits do not change its arithmetic decoding.
  if (function == 0x3Fu && fd == 0x0Bu) return true;

  const auto ft = static_cast<unsigned>((code >> 16) & 0x1Fu);
  const auto fs = static_cast<unsigned>((code >> 11) & 0x1Fu);
  const auto apply_product = [&](unsigned component, bool add,
                                 bool accumulator) {
    const auto scalar = as_float(state_.vf[ft][component]);
    const auto destination = static_cast<unsigned>((code >> 6) & 0x1Fu);
    for (unsigned lane = 0; lane < 4u; ++lane) {
      if ((code & (1u << (24u - lane))) == 0u) {
        state_.mac &= static_cast<std::uint16_t>(~(0x1111u << (3u - lane)));
        continue;
      }
      auto value = as_float(state_.vf[fs][lane]) * scalar;
      if (add) value += as_float(state_.acc[lane]);
      const auto bits = update_mac(state_.mac, lane, as_bits(value));
      if (accumulator) state_.acc[lane] = bits;
      else if (destination != 0u) state_.vf[destination][lane] = bits;
    }
  };

  if (function >= 0x3Cu) {
    const auto component = function - 0x3Cu;
    if (fd == 0x06u) { // MULAx/y/z/w
      apply_product(component, false, true);
      return true;
    }
    if (fd == 0x02u) { // MADDAx/y/z/w
      apply_product(component, true, true);
      return true;
    }
    if (fd == 0x05u) { // FTOI0 / FTOI4 / FTOI12 / FTOI15
      constexpr unsigned scales[4] = {0u, 4u, 12u, 15u};
      if (ft != 0u) {
        for (unsigned lane = 0; lane < 4u; ++lane) {
          if ((code & (1u << (24u - lane))) != 0u)
            state_.vf[ft][lane] = float_to_int(
                state_.vf[fs][lane], scales[function - 0x3Cu]);
        }
      }
      return true;
    }
  }
  if (function >= 0x08u && function <= 0x0Bu) { // MADDx/y/z/w
    apply_product(function - 0x08u, true, false);
    return true;
  }
  if (function == 0x1Cu) { // MULq
    const auto scalar = as_float(state_.q);
    if (fd != 0u) {
      for (unsigned lane = 0; lane < 4u; ++lane) {
        if ((code & (1u << (24u - lane))) != 0u) {
          state_.vf[fd][lane] = update_mac(state_.mac, lane,
              as_bits(as_float(state_.vf[fs][lane]) * scalar));
        } else {
          state_.mac &= static_cast<std::uint16_t>(~(0x1111u << (3u - lane)));
        }
      }
    }
    return true;
  }
  const bool add_broadcast = function <= 0x03u;
  const bool sub_broadcast = function >= 0x04u && function <= 0x07u;
  const bool max_broadcast = function >= 0x10u && function <= 0x13u;
  const bool vector_binary = function == 0x28u || function == 0x2Au ||
                             function == 0x2Bu || function == 0x2Cu;
  if (add_broadcast || sub_broadcast || max_broadcast || vector_binary) {
    const auto component = function & 3u;
    if (fd != 0u) {
      for (unsigned lane = 0; lane < 4u; ++lane) {
        if ((code & (1u << (24u - lane))) == 0u) {
          state_.mac &= static_cast<std::uint16_t>(~(0x1111u << (3u - lane)));
          continue;
        }
        const auto lhs = as_float(state_.vf[fs][lane]);
        const auto rhs = as_float(state_.vf[ft][vector_binary ? lane : component]);
        float result = 0.0f;
        if (add_broadcast || function == 0x28u) result = lhs + rhs;
        else if (sub_broadcast || function == 0x2Cu) result = lhs - rhs;
        else if (max_broadcast || function == 0x2Bu) result = std::fmax(lhs, rhs);
        else result = lhs * rhs;
        state_.vf[fd][lane] = update_mac(state_.mac, lane, as_bits(result));
      }
    }
    return true;
  }
  return false;
}

} // namespace ps2vita
