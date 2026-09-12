#include "ps2vita/vu.hpp"

#include <cmath>
#include <algorithm>
#include <cstring>
#include <limits>

namespace ps2vita {
namespace {
struct VectorAccess {
  std::array<unsigned, 32> reads{};
  unsigned destination = 0, mask = 0;
};

VectorAccess vector_access(std::uint32_t code, bool upper) {
  VectorAccess access;
  const unsigned mask = (code >> 21) & 15u, fs = (code >> 11) & 31u,
      ft = (code >> 16) & 31u, fd = (code >> 6) & 31u, fn = code & 63u;
  if (upper) {
    const bool broadcast = fn <= 0x0Bu || (fn >= 0x10u && fn <= 0x13u);
    const bool vector = fn == 0x28u || fn == 0x2Au || fn == 0x2Bu || fn == 0x2Cu;
    if (broadcast || vector || fn == 0x1Cu) {
      access.reads[fs] |= mask;
      if (broadcast && mask) access.reads[ft] |= 8u >> (fn & 3u);
      else if (vector) access.reads[ft] |= mask;
      access.destination = fd; access.mask = mask;
    } else if (fn >= 0x3Cu && (fd == 2u || fd == 6u || fd == 5u)) {
      access.reads[fs] |= mask;
      if (fd == 5u) { access.destination = ft; access.mask = mask; }
      else if (mask) access.reads[ft] |= 8u >> (fn & 3u);
    }
  } else {
    const unsigned group = code >> 25;
    if (group == 0u || (group == 0x40u &&
        ((fn == 0x3Cu && fd == 0xDu) || (fn == 0x3Du && fd == 0xFu)))) {
      access.destination = ft; access.mask = mask;
    } else if (group == 1u || (group == 0x40u && fn == 0x3Du && fd == 0xDu)) {
      access.reads[fs] |= mask;
    } else if (group == 0x40u && fn == 0x3Cu && fd == 0xEu) {
      access.reads[fs] |= 8u >> ((code >> 21) & 3u);
      access.reads[ft] |= 8u >> ((code >> 23) & 3u);
    } else if (group == 0x40u && fn == 0x3Cu && fd == 0xFu) {
      access.reads[fs] |= 8u >> ((code >> 21) & 3u);
    }
  }
  access.reads[0] = 0;
  return access;
}

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

void Vu1::enable_causal_trace(bool enabled) {
  trace_causes_ = enabled;
  causes_.clear(); vf_causes_ = {}; lower_causes_ = {};
  acc_causes_ = {}; data_causes_ = {}; rejected_causes_ = {};
  dropped_causes_ = 0;
}

void Vu1::invalidate_data_cause(std::uint32_t address) {
  if (trace_causes_)
    data_causes_[((address - Memory::kVu1DataBase) & 0x3FFFu) / 4u] = 0;
}

std::uint32_t Vu1::add_cause(VuCauseRecord record) {
  if (causes_.size() >= 8192u) { ++dropped_causes_; return 0; }
  record.pc = state_.pc; record.pair = pairs_executed_; record.cycle = cycles_;
  causes_.push_back(record);
  return static_cast<std::uint32_t>(causes_.size()); // Zero is explicitly unknown.
}

void Vu1::trace_upper(std::uint32_t code) {
  const unsigned fn = code & 63u, fd = (code >> 6) & 31u,
      fs = (code >> 11) & 31u, ft = (code >> 16) & 31u;
  const bool acc = fn >= 0x3Cu && (fd == 2u || fd == 6u);
  const bool convert = fn >= 0x3Cu && fd == 5u;
  const auto access = vector_access(code, true);
  const unsigned dest = acc ? 0u : access.destination;
  if (!acc && dest == 0u) return;
  for (unsigned lane = 0; lane < 4; ++lane) {
    if ((code & (1u << (24u - lane))) == 0u) continue;
    VuCauseRecord record;
    record.instruction = code; record.reg = dest; record.lane = lane;
    record.accumulator = acc;
    record.mask = (code >> 21) & 15u;
    record.value = acc ? state_.acc[lane] : state_.vf[dest][lane];
    record.parents[0] = lower_causes_[fs][lane];
    record.incomplete = record.parents[0] == 0;
    if (!convert && fn != 0x1Cu) {
      const unsigned component = (acc || fn <= 0x13u) ? fn & 3u : lane;
      record.parents[1] = lower_causes_[ft][component];
      record.incomplete |= record.parents[1] == 0;
    }
    if ((fn >= 8u && fn <= 11u) || (acc && fd == 2u)) {
      record.parents[2] = acc_causes_[lane];
      record.incomplete |= record.parents[2] == 0;
    }
    if (fn == 0x1Cu) record.incomplete = true; // Q ancestry is not modeled yet.
    const auto id = add_cause(record);
    if (acc) acc_causes_[lane] = id;
    else vf_causes_[dest][lane] = id;
  }
}

void Vu1::reset() {
  enable_causal_trace(trace_causes_);
  vf_ready_ = {}; cycles_ = vf_stall_cycles_ = 0;
  q_ready_ = q_stall_cycles_ = 0; pending_q_ = 0; q_pending_ = false;
  store_records_.clear(); dropped_store_records_ = 0; first_rejected_pair_ = 0;
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
  first_rejected_tag_ = 0;
  first_rejected_address_ = 0;
  first_rejected_pc_ = first_rejected_kick_start_ = 0;
  first_rejected_tag_index_ = 0;
  first_rejected_previous_tag_ = 0;
  first_rejected_data_.fill(0u);
  top_ = 0;
  mac_pipeline_.fill(0u);
  mac_pipeline_slot_ = 0u;
  path1_packets_.clear();
  kick_active_ = kick_eop_ = false;
  kick_offset_ = kick_pc_ = 0;
  kick_tag_index_ = kick_remaining_ = 0;
  kick_next_cycle_ = kick_previous_tag_ = kick_tag_ = xgkick_stall_cycles_ = 0;
  kick_packet_.clear();
}

void Vu1::start(std::uint16_t address) {
  mac_pipeline_.fill(state_.mac);
  mac_pipeline_slot_ = 0u;
  state_.pc = address & 0x3FF8u;
  running_ = true;
  branch_pending_ = false;
  end_pending_ = false;
}

void Vu1::resume() {
  mac_pipeline_.fill(state_.mac);
  mac_pipeline_slot_ = 0u;
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
  const auto upper_access = vector_access(upper, true);
  const auto lower_access = (upper & 0x80000000u) ? VectorAccess{} : vector_access(lower, false);
  auto ready = cycles_;
  for (unsigned reg = 1; reg < 32; ++reg)
    for (unsigned lane = 0; lane < 4; ++lane)
      if (((upper_access.reads[reg] | lower_access.reads[reg]) & (8u >> lane)) != 0u)
        ready = std::max(ready, vf_ready_[reg][lane]);
  while (cycles_ < ready) {
    mac_pipeline_[mac_pipeline_slot_] = state_.mac;
    mac_pipeline_slot_ = (mac_pipeline_slot_ + 1u) & 3u;
    ++cycles_; ++vf_stall_cycles_;
  }
  const bool lower_div = (lower >> 25) == 0x40u && (lower & 0x7FFu) == 0x3BCu;
  const bool lower_waitq = (lower >> 25) == 0x40u && (lower & 0x7FFu) == 0x3BFu;
  if ((upper & 0x80000000u) == 0u && (lower_div || lower_waitq) && q_pending_) {
    while (cycles_ < q_ready_) {
      mac_pipeline_[mac_pipeline_slot_] = state_.mac;
      mac_pipeline_slot_ = (mac_pipeline_slot_ + 1u) & 3u;
      ++cycles_; ++q_stall_cycles_;
    }
  }
  transfer_path1(false);
  if (q_pending_ && cycles_ >= q_ready_) {
    state_.q = pending_q_;
    q_pending_ = false;
  }
  lower_mac_snapshot_ = mac_pipeline_[mac_pipeline_slot_];
  branch_pending_ = false;
  end_pending_ = (upper & 0x40000000u) != 0u;

  lower_vf_snapshot_ = state_.vf;
  current_lower_ = lower;
  if (trace_causes_) lower_causes_ = vf_causes_;
  if (!execute_upper(upper)) {
    first_unsupported_upper_ = upper;
    running_ = false;
    return false;
  }
  // When I is set, the lower word is the immediate register payload.
  if (trace_causes_) trace_upper(upper);
  if ((upper & 0x80000000u) != 0u) state_.i = lower;
  else {
    // The implemented upper writers use FD, except FTOI's FT. NOP and
    // accumulator-only operations do not write VF. Resolve same-pair write
    // conflicts by discarding the lower instruction, including its side effects.
    const unsigned fn = upper & 63u, fd = (upper >> 6) & 31u;
    unsigned upper_dest = fn < 0x3Cu ? fd : fd == 5u ? (upper >> 16) & 31u : 0u;
    if ((upper & 0x1E00000u) == 0u) upper_dest = 0u;
    const unsigned group = lower >> 25, lfn = lower & 63u, lfd = (lower >> 6) & 31u;
    const bool lower_load = group == 0u || (group == 0x40u &&
        ((lfn == 0x3Cu && lfd == 0x0Du) || (lfn == 0x3Du && lfd == 0x0Fu)));
    const unsigned lower_dest = lower_load ? (lower >> 16) & 31u : 0u;
    if ((upper_dest == 0u || upper_dest != lower_dest) && !execute_lower(lower)) {
      first_unsupported_lower_ = lower;
      running_ = false;
      return false;
    }
    if (trace_causes_ && lower_dest != 0u && (upper_dest == 0u || upper_dest != lower_dest)) {
      for (unsigned lane = 0; lane < 4; ++lane) {
        if ((lower & (1u << (24u - lane))) == 0u) continue;
        VuCauseRecord record;
        record.kind = VuCauseRecord::Kind::LowerInput;
        record.instruction = lower; record.reg = lower_dest; record.lane = lane;
        record.mask = (lower >> 21) & 15u;
        record.value = state_.vf[lower_dest][lane]; record.incomplete = true;
        vf_causes_[lower_dest][lane] = add_cause(record);
      }
    }
  }

  mac_pipeline_[mac_pipeline_slot_] = state_.mac;
  mac_pipeline_slot_ = (mac_pipeline_slot_ + 1u) & 3u;
  ++pairs_executed_;
  const auto mark_ready = [&](const VectorAccess& access) {
    if (access.destination == 0u) return;
    for (unsigned lane = 0; lane < 4; ++lane)
      if ((access.mask & (8u >> lane)) != 0u)
        vf_ready_[access.destination][lane] = cycles_ + 4u;
  };
  mark_ready(upper_access);
  if (upper_access.destination == 0u || upper_access.mask == 0u ||
      upper_access.destination != lower_access.destination)
    mark_ready(lower_access);
  ++cycles_;
  state_.pc = apply_branch ? pending_target : sequential_pc;
  if (apply_end) {
    // Retire a pending division after the E-bit delay pair, not when E is
    // first encountered. A host run budget is not a microprogram termination.
    if (q_pending_) {
      while (cycles_ < q_ready_) {
        mac_pipeline_[mac_pipeline_slot_] = state_.mac;
        mac_pipeline_slot_ = (mac_pipeline_slot_ + 1u) & 3u;
        ++cycles_; ++q_stall_cycles_;
      }
      state_.q = pending_q_;
      q_pending_ = false;
    }
    transfer_path1(true);
    running_ = false;
  }
  return true;
}

void Vu1::store_data(std::uint32_t address, std::uint32_t value, unsigned reg, unsigned lane) {
  memory_.write32(address, value);
  if (trace_causes_) {
    VuCauseRecord record;
    record.kind = VuCauseRecord::Kind::Store;
    record.instruction = current_lower_; record.value = value;
    record.address = (address - Memory::kVu1DataBase) & 0x3FFFu;
    record.reg = reg; record.lane = lane; record.mask = (current_lower_ >> 21) & 15u;
    record.parents[0] = lower_causes_[reg][lane];
    record.incomplete = record.parents[0] == 0;
    data_causes_[record.address / 4u] = add_cause(record);
  }
  if (!trace_stores_) return;
  if (store_records_.size() < 4096u)
    store_records_.push_back({pairs_executed_, cycles_, state_.pc,
        static_cast<std::uint16_t>((address - Memory::kVu1DataBase) & 0x3FFFu), value});
  else ++dropped_store_records_;
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
      if (store) store_data(address, lower_vf_snapshot_[vector_reg][lane], vector_reg, lane);
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
    const auto immediate = static_cast<std::int32_t>(raw);
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
    pending_q_ = as_bits(as_float(lower_vf_snapshot_[fs][(code >> 21) & 3u]) /
                       as_float(lower_vf_snapshot_[ft][(code >> 23) & 3u]));
    q_ready_ = cycles_ + 7u;
    q_pending_ = true;
    return true;
  }
  if (function == 0x3Cu && fd == 0x0Fu) { // MTIR
    if (it != 0u) state_.vi[it] = static_cast<std::uint16_t>(
        lower_vf_snapshot_[(code >> 11) & 0x1Fu][(code >> 21) & 3u]);
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
        store_data(Memory::kVu1DataBase + qword * 16u + lane * 4u,
                        lower_vf_snapshot_[fs][lane], fs, lane);
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
  transfer_path1(true);
  kick_offset_ = static_cast<std::uint16_t>((state_.vi[address_reg] & 0x3FFu) * 16u);
  last_kick_address_ = kick_offset_;
  last_kick_tag_ = 0;
  kick_pc_ = state_.pc;
  kick_tag_index_ = kick_remaining_ = 0;
  kick_previous_tag_ = 0;
  kick_packet_.clear();
  kick_active_ = true;
  // One qword per two modeled cycles, with the issue pair counting as one.
  // No GIF arbitration/backpressure is modeled here yet.
  kick_next_cycle_ = cycles_ + 2u;
  return true;
}

void Vu1::transfer_path1(bool flush) {
  while (kick_active_ && (flush || cycles_ >= kick_next_cycle_)) {
    if (cycles_ < kick_next_cycle_) {
      const auto wait = kick_next_cycle_ - cycles_;
      for (std::uint64_t n = 0; n < wait; ++n) {
        mac_pipeline_[mac_pipeline_slot_] = state_.mac;
        mac_pipeline_slot_ = (mac_pipeline_slot_ + 1u) & 3u;
      }
      cycles_ = kick_next_cycle_;
      xgkick_stall_cycles_ += wait;
    }
    std::array<std::uint8_t, 16> tag_bytes{};
    for (unsigned byte = 0; byte < tag_bytes.size(); ++byte)
      tag_bytes[byte] = memory_.read8(
          Memory::kVu1DataBase + ((kick_offset_ + byte) & 0x3FFFu));
    if (kick_packet_.empty()) {
      std::uint64_t tag = 0;
      std::memcpy(&tag, tag_bytes.data(), sizeof(tag));
      if (kick_tag_index_ == 0u) last_kick_tag_ = tag;

      const auto loops = static_cast<std::uint32_t>(tag & 0x7FFFu);
      const auto format = static_cast<unsigned>((tag >> 58) & 3u);
      auto registers = static_cast<std::uint32_t>((tag >> 60) & 0xFu);
      if (registers == 0u) registers = 16u;
      std::uint64_t payload_size = 0;
      if (format == 0u) payload_size = std::uint64_t{loops} * registers * 16u;
      else if (format == 1u)
        payload_size = ((std::uint64_t{loops} * registers + 1u) / 2u) * 16u;
      else payload_size = std::uint64_t{loops} * 16u;
      if (payload_size > 0x3FF0u || kick_tag_index_ >= 256u) {
        if (path1_tags_rejected_ == 0u) {
          first_rejected_pair_ = pairs_executed_;
          first_rejected_tag_ = tag;
          first_rejected_address_ = kick_offset_;
          first_rejected_pc_ = kick_pc_;
          first_rejected_kick_start_ = last_kick_address_;
          first_rejected_tag_index_ = kick_tag_index_;
          first_rejected_previous_tag_ = kick_previous_tag_;
          if (trace_causes_)
            for (unsigned lane = 0; lane < 4; ++lane)
              rejected_causes_[lane] = data_causes_[((kick_offset_ + lane * 4u) & 0x3FFFu) / 4u];
          for (unsigned word = 0; word < first_rejected_data_.size(); ++word)
            first_rejected_data_[word] = memory_.read32(Memory::kVu1DataBase +
                ((last_kick_address_ + word * 4u) & 0x3FFFu));
        }
        ++path1_tags_rejected_;
        kick_active_ = false;
        return;
      }
      kick_remaining_ = static_cast<unsigned>(payload_size / 16u);
      kick_eop_ = (tag & (1ull << 15)) != 0u;
      kick_tag_ = tag;
    } else {
      --kick_remaining_;
    }
    kick_packet_.insert(kick_packet_.end(), tag_bytes.begin(), tag_bytes.end());
    kick_offset_ = (kick_offset_ + 16u) & 0x3FFFu;
    kick_next_cycle_ += 2u;
    if (kick_remaining_ == 0u) {
      path1_packets_.push_back(std::move(kick_packet_));
      kick_packet_.clear();
      ++path1_tags_queued_;
      ++kick_tag_index_;
      kick_previous_tag_ = kick_tag_;
      if (kick_eop_) kick_active_ = false;
    }
  }
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
    const auto broadcast = as_float(state_.vf[ft][component]);
    const bool changes_mac = !max_broadcast && function != 0x2Bu;
    if (fd != 0u) {
      for (unsigned lane = 0; lane < 4u; ++lane) {
        if ((code & (1u << (24u - lane))) == 0u) {
          if (changes_mac)
            state_.mac &= static_cast<std::uint16_t>(~(0x1111u << (3u - lane)));
          continue;
        }
        const auto lhs = as_float(state_.vf[fs][lane]);
        const auto rhs = vector_binary ? as_float(state_.vf[ft][lane]) : broadcast;
        float result = 0.0f;
        if (add_broadcast || function == 0x28u) result = lhs + rhs;
        else if (sub_broadcast || function == 0x2Cu) result = lhs - rhs;
        else if (max_broadcast || function == 0x2Bu) result = std::fmax(lhs, rhs);
        else result = lhs * rhs;
        state_.vf[fd][lane] = changes_mac ?
            update_mac(state_.mac, lane, as_bits(result)) : as_bits(result);
      }
    }
    return true;
  }
  return false;
}

} // namespace ps2vita
