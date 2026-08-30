#include "ps2vita/memory.hpp"

#include <algorithm>
#include <cstring>

namespace ps2vita {

Memory::Memory()
    : ram_(kRamSize, 0), bios_(kBiosSize, 0), scratch_(kScratchSize, 0),
      hw_(kHwSize, 0), gs_hw_(kGsHwSize, 0), vu_mem_(kVuSize, 0),
      iop_ram_(kIopRamSize, 0),
      iop_hw_(kIopHwSize, 0),
      ee_internal_(kEeInternalSize, 0), dve_(kDveSize, 0) {
  clear();
}

std::uint32_t Memory::physical(std::uint32_t address) const {
  // EE cached/uncached kernel segments alias the first 512 MiB.
  if ((address & 0xE0000000u) == 0x80000000u ||
      (address & 0xE0000000u) == 0xA0000000u) {
    return address & 0x1FFFFFFFu;
  }
  // KSEG2/KSEG3 and user mappings are translated through the EE TLB. This
  // implements the common MIPS paired-page format, including variable masks.
  for (const auto& entry : tlb_) {
    if (!entry.written) continue;
    // The R5900's S bit in EntryLo0 selects the 16 KiB on-chip scratchpad.
    // Unlike an ordinary paired-page entry, the complete aligned 16 KiB
    // virtual aperture is mapped to scratchpad; EntryLo1 does not describe a
    // second RAM page.  The retail BIOS uses 0x80000007/0x00000007 here.
    if (entry.entry_lo0 & 0x80000000u) {
      constexpr std::uint32_t scratch_mask = kScratchSize - 1u;
      const std::uint32_t virtual_base = entry.entry_hi & ~scratch_mask;
      if ((address & ~scratch_mask) == virtual_base)
        return kScratchBase + ((address - virtual_base) & scratch_mask);
      continue;
    }
    const std::uint32_t pair_mask = (entry.page_mask & 0x01FFE000u) | 0x1FFFu;
    if ((address & ~pair_mask) != (entry.entry_hi & ~pair_mask)) continue;
    const std::uint32_t single_page_size = (pair_mask + 1u) >> 1;
    const bool odd = (address & single_page_size) != 0;
    const std::uint32_t lo = odd ? entry.entry_lo1 : entry.entry_lo0;
    if ((lo & 2u) == 0) return address; // Invalid mapping; caller will fault.
    const std::uint32_t offset_mask = single_page_size - 1u;
    const std::uint32_t page_base = ((lo >> 6) << 12) & ~offset_mask;
    return page_base | (address & offset_mask);
  }
  return address;
}

void Memory::clear_tlb() {
  tlb_ = {};
  ++mapping_generation_;
}

void Memory::write_tlb(unsigned index, std::uint32_t page_mask,
                       std::uint32_t entry_hi, std::uint32_t entry_lo0,
                       std::uint32_t entry_lo1) {
  if (index >= tlb_.size()) return;
  tlb_[index] = {page_mask, entry_hi, entry_lo0, entry_lo1, true};
  ++mapping_generation_;
}

bool Memory::read_tlb(unsigned index, std::uint32_t& page_mask,
                      std::uint32_t& entry_hi, std::uint32_t& entry_lo0,
                      std::uint32_t& entry_lo1) const {
  if (index >= tlb_.size() || !tlb_[index].written) return false;
  const auto& entry = tlb_[index];
  page_mask = entry.page_mask; entry_hi = entry.entry_hi;
  entry_lo0 = entry.entry_lo0; entry_lo1 = entry.entry_lo1;
  return true;
}

int Memory::probe_tlb(std::uint32_t entry_hi) const {
  for (unsigned i = 0; i < tlb_.size(); ++i) {
    if (!tlb_[i].written) continue;
    const std::uint32_t pair_mask = (tlb_[i].page_mask & 0x01FFE000u) | 0x1FFFu;
    if ((entry_hi & ~pair_mask) == (tlb_[i].entry_hi & ~pair_mask))
      return static_cast<int>(i);
  }
  return -1;
}

void Memory::clear() {
  std::fill(ram_.begin(), ram_.end(), 0);
  std::fill(scratch_.begin(), scratch_.end(), 0);
  std::fill(hw_.begin(), hw_.end(), 0);
  std::fill(gs_hw_.begin(), gs_hw_.end(), 0);
  std::fill(vu_mem_.begin(), vu_mem_.end(), 0);
  std::fill(iop_ram_.begin(), iop_ram_.end(), 0);
  iop_scratch_.fill(0);
  std::fill(iop_hw_.begin(), iop_hw_.end(), 0);
  std::fill(ee_internal_.begin(), ee_internal_.end(), 0);
  std::fill(dve_.begin(), dve_.end(), 0);
  dve_device_regs_ = {};
  dve_current_reg_ = 0;
  dve_command_executing_ = false;
  dve_error_ = false;
  for (auto& generation : ram_page_generation_) ++generation;
  timer0_count_ = 0;
  timer0_reads_ = 0;
  rdram_sdevid_ = 0;
  iop_cache_control_ = 0;
  // EE hardware reset values observed by the BIOS during board detection.
  write32(0x1000F260u, 0x1D000060u);
  write32(0x1000F590u, 0x00001201u);
}

std::uint32_t Memory::page_generation(std::uint32_t address) const {
  const auto p = physical(address);
  if (p < kRamSize)
    return ram_page_generation_[p >> 12] ^ mapping_generation_;
  return mapping_generation_;
}

bool Memory::valid(std::uint32_t address, std::size_t size) const {
  if (address >= kEeInternalBase) {
    const auto offset = static_cast<std::size_t>(address - kEeInternalBase);
    if (offset <= ee_internal_.size() && size <= ee_internal_.size() - offset)
      return true;
  }
  if (address >= kDevBoardBase && address < kDevBoardBase + kDevBoardSize &&
      size <= kDevBoardBase + kDevBoardSize - address)
    return true;
  const auto p = static_cast<std::size_t>(physical(address));
  if (p <= ram_.size() && size <= ram_.size() - p) return true;
  if (p >= kNullBase && p <= kNullEnd && size <= kNullEnd - p) return true;
  if (p >= kBiosBase) {
    const auto offset = p - kBiosBase;
    if (offset <= bios_.size() && size <= bios_.size() - offset) return bios_loaded_;
  }
  if (address >= kScratchBase) {
    const auto offset = static_cast<std::size_t>(address - kScratchBase);
    if (offset <= scratch_.size() && size <= scratch_.size() - offset) return true;
  }
  if (p >= kHwBase) {
    const auto offset = static_cast<std::size_t>(p - kHwBase);
    if (offset <= hw_.size() && size <= hw_.size() - offset) return true;
  }
  if (p >= kVuBase && p < kVuBase + kVuSize) {
    const auto offset = static_cast<std::size_t>(p - kVuBase);
    const auto within = [&](std::size_t begin, std::size_t end) {
      return offset >= begin && offset <= end && size <= end - offset;
    };
    if (within(0x0000u, 0x1000u) || within(0x4000u, 0x5000u) ||
        within(0x8000u, 0xC000u) || within(0xC000u, 0x10000u))
      return true;
  }
  if (p >= kGsHwBase) {
    const auto offset = static_cast<std::size_t>(p - kGsHwBase);
    if (offset <= gs_hw_.size() && size <= gs_hw_.size() - offset) return true;
  }
  if (p >= kDveBase) {
    const auto offset = static_cast<std::size_t>(p - kDveBase);
    if (offset <= dve_.size() && size <= dve_.size() - offset) return true;
  }
  if (p >= kIopBase) {
    const auto offset = static_cast<std::size_t>(p - kIopBase);
    if (offset <= kIopWindowSize && size <= kIopWindowSize - offset) return true;
  }
  if (p >= kIopHwBase) {
    const auto offset = static_cast<std::size_t>(p - kIopHwBase);
    if (offset <= iop_hw_.size() && size <= iop_hw_.size() - offset) return true;
  }
  return false;
}

std::uint8_t Memory::read8(std::uint32_t address) const {
  const auto p = physical(address);
  if (!valid(address)) return 0;
  if (address >= kEeInternalBase)
    return ee_internal_[address - kEeInternalBase];
  if (address >= kDevBoardBase && address < kDevBoardBase + kDevBoardSize)
    return 0;
  if (p < ram_.size()) return ram_[p];
  if (p >= kNullBase && p < kNullEnd) return 0;
  if (p >= kBiosBase && p < kBiosBase + kBiosSize) return bios_[p - kBiosBase];
  if (address >= kScratchBase && address < kScratchBase + kScratchSize)
    return scratch_[address - kScratchBase];
  if (p >= kHwBase && p < kHwBase + kHwSize) return hw_[p - kHwBase];
  if (p >= kVuBase && p < kVuBase + kVuSize)
    return vu_mem_[p - kVuBase];
  if (p >= kGsHwBase && p < kGsHwBase + kGsHwSize)
    return gs_hw_[p - kGsHwBase];
  if (p >= kDveBase && p < kDveBase + kDveSize)
    return dve_[p - kDveBase];
  if (p >= kIopHwBase && p < kIopHwBase + kIopHwSize)
    return iop_hw_[p - kIopHwBase];
  return iop_ram_[(p - kIopBase) & (kIopRamSize - 1u)];
}

std::uint16_t Memory::read16(std::uint32_t address) const {
  const auto p = physical(address);
  if (p >= kDveBase && p < kDveBase + kDveSize) {
    const auto offset = static_cast<std::size_t>(p - kDveBase);
    const auto raw = [&](std::size_t at) {
      return static_cast<std::uint16_t>(dve_[at]) |
             (static_cast<std::uint16_t>(dve_[at + 1]) << 8);
    };
    if ((offset & 0xFFu) == 0x06u) {
      auto status = static_cast<std::uint16_t>(raw(offset) & 2u);
      if (dve_error_) status |= 1u;
      auto current = raw(offset);
      if (static_cast<std::uint16_t>(current) < std::uint16_t{3} &&
          dve_command_executing_) {
        ++current;
        dve_[offset] = static_cast<std::uint8_t>(current);
        dve_[offset + 1] = static_cast<std::uint8_t>(current >> 8);
      } else {
        dve_command_executing_ = false;
      }
      return status;
    }
    return raw(offset);
  }
  return static_cast<std::uint16_t>(read8(address)) |
         (static_cast<std::uint16_t>(read8(address + 1)) << 8);
}

std::uint32_t Memory::read32(std::uint32_t address) const {
  const auto p = physical(address);
  // SBUS control reads expose fixed hardware-identification/status bits in
  // addition to the writable IOP-control bit.
  if (p == 0x1000F240u) {
    const auto offset = static_cast<std::size_t>(p - kHwBase);
    const auto raw = static_cast<std::uint32_t>(hw_[offset]) |
        (static_cast<std::uint32_t>(hw_[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(hw_[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(hw_[offset + 3]) << 24);
    return raw | 0xF0000102u;
  }
  // Timer 0 COUNT. Until the scheduler owns this clock, prescale polling reads
  // so the BIOS can measure a meaningful EE/Timer ratio instead of observing a
  // timer transition on every load instruction.
  if (p == kHwBase) {
    constexpr unsigned kReadsPerTick = 32;
    const auto value = timer0_count_;
    if (++timer0_reads_ == kReadsPerTick) {
      timer0_reads_ = 0;
      ++timer0_count_;
    }
    return value;
  }
  // Memory-controller command ports complete synchronously in this model.
  if (p == 0x1000F410u || p == 0x1000F430u) return 0;
  if (p == 0x1000F440u) { // MCH_DRD: RDRAM serial command response.
    const auto at = [](const std::vector<std::uint8_t>& bytes, std::size_t offset) {
      return static_cast<std::uint32_t>(bytes[offset]) |
             (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
             (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
             (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
    };
    const auto ricm = at(hw_, 0xF430u);
    if (((ricm >> 6) & 0xFu) == 0) {
      switch ((ricm >> 16) & 0xFFFu) {
      case 0x21: // INIT enumerates the two retail-console RDRAM devices.
        if (rdram_sdevid_ < 2) { ++rdram_sdevid_; return 0x1Fu; }
        return 0;
      case 0x23: return 0x0D0Du; // CNFGA
      case 0x24: return 0x0090u; // CNFGB
      case 0x40: return ricm & 0x1Fu; // DEVID
      default: break;
      }
    }
    return 0;
  }
  return static_cast<std::uint32_t>(read16(address)) |
         (static_cast<std::uint32_t>(read16(address + 2)) << 16);
}

std::uint64_t Memory::read64(std::uint32_t address) const {
  return static_cast<std::uint64_t>(read32(address)) |
         (static_cast<std::uint64_t>(read32(address + 4)) << 32);
}

void Memory::write8(std::uint32_t address, std::uint8_t value) {
  const auto p = physical(address);
  if (address >= kEeInternalBase)
    ee_internal_[address - kEeInternalBase] = value;
  else if (address >= kDevBoardBase && address < kDevBoardBase + kDevBoardSize)
    return;
  else if (p < ram_.size()) {
    ram_[p] = value;
    ++ram_page_generation_[p >> 12];
  }
  else if (p >= kNullBase && p < kNullEnd) return;
  else if (address >= kScratchBase && address < kScratchBase + kScratchSize)
    scratch_[address - kScratchBase] = value;
  else if (p >= kHwBase && p < kHwBase + kHwSize) {
    // INTC_STAT and DMAC_STAT acknowledge pending bits when written. Byte writes
    // are retained here; wider accessors below apply the register semantics.
    hw_[p - kHwBase] = value;
  }
  else if (p >= kVuBase && p < kVuBase + kVuSize)
    vu_mem_[p - kVuBase] = value;
  else if (p >= kGsHwBase && p < kGsHwBase + kGsHwSize)
    gs_hw_[p - kGsHwBase] = value;
  else if (p >= kDveBase && p < kDveBase + kDveSize)
    dve_[p - kDveBase] = value;
  else if (p >= kIopHwBase && p < kIopHwBase + kIopHwSize)
    iop_hw_[p - kIopHwBase] = value;
  else if (p >= kIopBase && p < kIopBase + kIopWindowSize)
    iop_ram_[(p - kIopBase) & (kIopRamSize - 1u)] = value;
}

void Memory::write16(std::uint32_t address, std::uint16_t value) {
  const auto p = physical(address);
  if (p >= kDveBase && p + 1u < kDveBase + kDveSize) {
    const auto offset = static_cast<std::size_t>(p - kDveBase);
    const auto slot = offset & 0xFFu;
    const auto raw = [&](std::size_t at) {
      return static_cast<std::uint16_t>(dve_[at]) |
             (static_cast<std::uint16_t>(dve_[at + 1]) << 8);
    };
    const auto store = [&](std::size_t at, std::uint16_t data) {
      dve_[at] = static_cast<std::uint8_t>(data);
      dve_[at + 1] = static_cast<std::uint8_t>(data >> 8);
    };
    if (slot == 0x06u) store(offset, raw(offset) & ~3u);
    else store(offset, value);
    if (slot == 0x00u) {
      const auto mode = raw((offset & ~0xFFu) + 0x02u);
      if (mode == 0x4Fu || mode == 0x41u) {
        dve_error_ = true;
      } else if (value & 0x80u) {
        const unsigned size = value & 0xFu;
        const auto page = offset & ~0xFFu;
        dve_current_reg_ = static_cast<std::uint8_t>(raw(page + 0x10u));
        if (mode == 0x43u) {
          for (unsigned i = 0; i + 1u < size; ++i)
            dve_device_regs_[(dve_current_reg_ + i) & 0xFFu] =
                raw(page + 0x12u + i * 2u);
        } else if (mode == 0x42u) {
          for (unsigned i = 0; i < size; ++i)
            store(page + 0x10u + i * 2u,
                  dve_device_regs_[(dve_current_reg_ + i) & 0xFFu]);
        }
        dve_command_executing_ = true;
        dve_error_ = false;
      }
    } else if (slot == 0x0Au) {
      dve_error_ = value == 0;
    }
    return;
  }
  write8(address, static_cast<std::uint8_t>(value));
  write8(address + 1, static_cast<std::uint8_t>(value >> 8));
}

void Memory::write32(std::uint32_t address, std::uint32_t value) {
  const auto p = physical(address);
  // EE-side SIF flags are asymmetric: EE writes set MSFLAG and clear SMFLAG.
  if (p == 0x1000F220u) {
    value = read32(address) | value;
  } else if (p == 0x1000F230u) {
    value = read32(address) & ~value;
  } else if (p == 0x1000F240u) {
    const auto offset = static_cast<std::size_t>(p - kHwBase);
    const auto current = static_cast<std::uint32_t>(hw_[offset]) |
        (static_cast<std::uint32_t>(hw_[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(hw_[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(hw_[offset + 3]) << 24);
    value = (current & ~0x100u) | (value & 0x100u);
  }
  // DMAC_ENABLEW is the write port for the read-only DMAC_ENABLER mirror.
  // The BIOS uses the mirrored value as a hardware/board revision key while
  // selecting its RDRAM configuration table.
  if (p == 0x1000F590u) {
    write16(0x1000F520u, static_cast<std::uint16_t>(value));
    write16(0x1000F522u, static_cast<std::uint16_t>(value >> 16));
  }
  if (p == 0x1000F430u) { // MCH_RICM
    const auto drd_offset = 0xF440u;
    const auto drd = static_cast<std::uint32_t>(hw_[drd_offset]) |
        (static_cast<std::uint32_t>(hw_[drd_offset + 1]) << 8) |
        (static_cast<std::uint32_t>(hw_[drd_offset + 2]) << 16) |
        (static_cast<std::uint32_t>(hw_[drd_offset + 3]) << 24);
    if (((value >> 16) & 0xFFFu) == 0x21u &&
        ((value >> 6) & 0xFu) == 1u && ((drd >> 7) & 1u) == 0)
      rdram_sdevid_ = 0;
    value &= ~0x80000000u;
  }
  if (p == 0x1000F000u || p == 0x1000E010u) {
    if (!valid(address, 4)) return;
    const auto current = read32(address);
    value = current & ~value;
  } else if (p == 0x1000F010u) {
    if (!valid(address, 4)) return;
    value = read32(address) ^ value; // EE INTC mask bits toggle on write.
  }
  write16(address, static_cast<std::uint16_t>(value));
  write16(address + 2, static_cast<std::uint16_t>(value >> 16));
}

void Memory::write64(std::uint32_t address, std::uint64_t value) {
  write32(address, static_cast<std::uint32_t>(value));
  write32(address + 4, static_cast<std::uint32_t>(value >> 32));
}

void Memory::advance(std::uint32_t cycles) {
  (void)cycles;
}

bool Memory::iop_valid(std::uint32_t address, std::size_t size) const {
  const auto p = static_cast<std::size_t>(address & 0x1FFFFFFFu);
  if (p < kIopWindowSize && size <= kIopWindowSize - p) return true;
  if (p >= kBiosBase) {
    const auto offset = p - kBiosBase;
    if (offset <= bios_.size() && size <= bios_.size() - offset)
      return bios_loaded_;
  }
  if (p >= kIopHwBase) {
    const auto offset = p - kIopHwBase;
    if (offset <= iop_hw_.size() && size <= iop_hw_.size() - offset)
      return true;
  }
  if (p >= 0x1F800000u && p < 0x1F801000u)
    return size <= 0x1F801000u - p;
  if (p == 0x1FFE0130u) return size <= 4;
  if (p >= 0x1D000000u && p <= 0x1D000060u)
    return size <= 0x1D000064u - p;
  return false;
}

std::uint8_t Memory::iop_read8(std::uint32_t address) const {
  const auto p = address & 0x1FFFFFFFu;
  if (p < kIopWindowSize) return iop_ram_[p & (kIopRamSize - 1u)];
  if (p >= kBiosBase && p < kBiosBase + kBiosSize && bios_loaded_)
    return bios_[p - kBiosBase];
  if (p >= 0x1D000000u && p <= 0x1D000063u) {
    const auto slot = p & 0x70u;
    if (slot <= 0x60u) {
      const auto word = read32(0x1000F200u + slot);
      return static_cast<std::uint8_t>(word >> ((p & 3u) * 8u));
    }
  }
  if (p >= 0x1F800000u && p < 0x1F801000u)
    return iop_scratch_[p - 0x1F800000u];
  if (p >= 0x1FFE0130u && p < 0x1FFE0134u)
    return static_cast<std::uint8_t>(iop_cache_control_ >> ((p & 3u) * 8u));
  if (p >= kIopHwBase && p < kIopHwBase + kIopHwSize)
    return iop_hw_[p - kIopHwBase];
  return 0;
}

std::uint16_t Memory::iop_read16(std::uint32_t address) const {
  return static_cast<std::uint16_t>(iop_read8(address)) |
      (static_cast<std::uint16_t>(iop_read8(address + 1u)) << 8);
}

std::uint32_t Memory::iop_read32(std::uint32_t address) const {
  return static_cast<std::uint32_t>(iop_read16(address)) |
      (static_cast<std::uint32_t>(iop_read16(address + 2u)) << 16);
}

void Memory::iop_write8(std::uint32_t address, std::uint8_t value) {
  const auto p = address & 0x1FFFFFFFu;
  if (p < kIopWindowSize) {
    iop_ram_[p & (kIopRamSize - 1u)] = value;
  } else if (p >= 0x1F800000u && p < 0x1F801000u) {
    iop_scratch_[p - 0x1F800000u] = value;
  } else if (p >= 0x1FFE0130u && p < 0x1FFE0134u) {
    const unsigned shift = (p & 3u) * 8u;
    iop_cache_control_ = (iop_cache_control_ & ~(0xFFu << shift)) |
        (static_cast<std::uint32_t>(value) << shift);
  } else if (p >= kIopHwBase && p < kIopHwBase + kIopHwSize) {
    iop_hw_[p - kIopHwBase] = value;
  }
}

void Memory::iop_write16(std::uint32_t address, std::uint16_t value) {
  iop_write8(address, static_cast<std::uint8_t>(value));
  iop_write8(address + 1u, static_cast<std::uint8_t>(value >> 8));
}

void Memory::iop_write32(std::uint32_t address, std::uint32_t value) {
  const auto p = address & 0x1FFFFFFFu;
  if (p >= 0x1D000000u && p <= 0x1D000060u) {
    const auto slot = p & 0x70u;
    const auto raw = [&](std::size_t offset) {
      return static_cast<std::uint32_t>(hw_[offset]) |
          (static_cast<std::uint32_t>(hw_[offset + 1]) << 8) |
          (static_cast<std::uint32_t>(hw_[offset + 2]) << 16) |
          (static_cast<std::uint32_t>(hw_[offset + 3]) << 24);
    };
    const auto store = [&](std::size_t offset, std::uint32_t data) {
      hw_[offset] = static_cast<std::uint8_t>(data);
      hw_[offset + 1] = static_cast<std::uint8_t>(data >> 8);
      hw_[offset + 2] = static_cast<std::uint8_t>(data >> 16);
      hw_[offset + 3] = static_cast<std::uint8_t>(data >> 24);
    };
    switch (slot) {
    case 0x10u: store(0xF210u, value); return;
    case 0x20u: store(0xF220u, raw(0xF220u) & ~value); return;
    case 0x30u: store(0xF230u, raw(0xF230u) | value); return;
    case 0x40u: {
      auto control = raw(0xF240u);
      const auto toggles = value & 0xF0u;
      control ^= toggles;
      if (value & 0xA0u) control = (control & ~0xF000u) | 0x2000u;
      store(0xF240u, control);
      return;
    }
    case 0x60u: store(0xF260u, 0); return;
    default: return;
    }
  }
  iop_write16(address, static_cast<std::uint16_t>(value));
  iop_write16(address + 2u, static_cast<std::uint16_t>(value >> 16));
}

bool Memory::copy_in(std::uint32_t address, const void* source, std::size_t size) {
  const auto p = physical(address);
  if (p > ram_.size() || size > ram_.size() - p) return false;
  std::memcpy(ram_.data() + p, source, size);
  if (size != 0) {
    const auto last = static_cast<std::uint32_t>(p + size - 1u);
    for (auto page = static_cast<std::uint32_t>(p) >> 12;
         page <= (last >> 12); ++page)
      ++ram_page_generation_[page];
  }
  return true;
}

bool Memory::zero(std::uint32_t address, std::size_t size) {
  const auto p = physical(address);
  if (p > ram_.size() || size > ram_.size() - p) return false;
  std::memset(ram_.data() + p, 0, size);
  if (size != 0) {
    const auto last = static_cast<std::uint32_t>(p + size - 1u);
    for (auto page = static_cast<std::uint32_t>(p) >> 12;
         page <= (last >> 12); ++page)
      ++ram_page_generation_[page];
  }
  return true;
}

bool Memory::load_bios(const void* source, std::size_t size) {
  if (!source || size != kBiosSize) return false;
  std::memcpy(bios_.data(), source, size);
  bios_loaded_ = true;
  return true;
}

} // namespace ps2vita
