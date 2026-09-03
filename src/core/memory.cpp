#include "ps2vita/memory.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

namespace ps2vita {

namespace {
// The BIOS configures NTSC interlace. These integer deadlines follow the
// 294.912 MHz EE clock at 59.94 fields/s and PCSX2's measured 22.5-scanline
// VBlank interval. A rational remainder corrects the fractional field cycle.
constexpr std::uint32_t kNtscRenderCycles = 4498396u;
constexpr std::uint32_t kNtscVblankCycles = 421724u;
constexpr std::uint32_t kNtscFieldRemainder = 360u;
constexpr std::uint32_t kNtscFieldDivisor = 2997u;
constexpr std::uint32_t kNtscScanlineCycles = 18743u;
constexpr std::uint32_t kNtscScanlineRemainder = 495225u;
constexpr std::uint32_t kNtscScanlineDivisor = 1573425u;
} // namespace

Memory::Memory()
    : ram_(kRamSize, 0), bios_(kBiosSize, 0), scratch_(kScratchSize, 0),
      hw_(kHwSize, 0), gs_hw_(kGsHwSize, 0), vu_mem_(kVuSize, 0),
      iop_ram_(kIopRamSize, 0), spu2_ram_(2u * 1024u * 1024u, 0),
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
  spu2_hw_.fill(0);
  // Both SPU2 cores reset ready. STATX bit 7 is cleared while a DMA transfer
  // is active and restored when the transfer completes.
  spu2_hw_[0x0344u] = 0x80u;
  spu2_hw_[0x0744u] = 0x80u;
  std::fill(spu2_ram_.begin(), spu2_ram_.end(), 0);
  iop_scratch_.fill(0);
  std::fill(iop_hw_.begin(), iop_hw_.end(), 0);
  // SIO2 reset state: no controller/memory-card devices are attached. The
  // command status still advertises a completed serial transaction state.
  iop_hw_[0x7268u] = 0xBCu;
  iop_hw_[0x7269u] = 0x03u;
  iop_hw_[0x726Cu] = 0x00u;
  iop_hw_[0x726Du] = 0xD1u;
  iop_hw_[0x726Eu] = 0x01u;
  iop_hw_[0x7270u] = 0x0Fu;
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
  cdvd_scmd_params_.fill(0);
  cdvd_scmd_result_.fill(0);
  cdvd_scmd_param_count_ = 0;
  cdvd_scmd_result_pos_ = 0;
  cdvd_scmd_result_count_ = 0;
  cdvd_sready_ = 0x40u;
  cdvd_config_rw_ = 0;
  cdvd_config_offset_ = 0;
  cdvd_config_blocks_ = 0;
  cdvd_config_index_ = 0;
  video_cycles_remaining_ = kNtscRenderCycles;
  video_field_remainder_ = 0;
  video_in_vblank_ = false;
  hblank_cycles_remaining_ = kNtscScanlineCycles;
  hblank_cycle_remainder_ = 0;
  timer3_target_future_ = false;
  iop_cycle_remainder_ = 0;
  timer5_prescale_remainder_ = 0;
  timer5_target_future_ = false;
  sif0_cycles_remaining_ = 0;
  sif1_cycles_remaining_ = 0;
  gif_cycles_remaining_ = 0;
  gif_dma_source_ = 0;
  gif_dma_qwc_ = 0;
  gif_packets_.clear();
  vif1_cycles_remaining_ = 0;
  vif1_final_tadr_ = 0;
  vif1_final_madr_ = 0;
  vif1_packets_.clear();
  spu2_dma_cycles_remaining_.fill(0);
  spu2_dma_source_.fill(0);
  spu2_dma_target_.fill(0);
  spu2_dma_bytes_.fill(0);
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
  if (p >= kCdvdBase && p < kCdvdBase + kCdvdSize)
    return size <= kCdvdBase + kCdvdSize - p;
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
  if (p >= kCdvdBase && p < kCdvdBase + kCdvdSize)
    return iop_read8(p);
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
  else if (p >= kCdvdBase && p < kCdvdBase + kCdvdSize)
    iop_write8(p, value);
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
  if (p == 0x1000F000u) {
    if (!valid(address, 4)) return;
    const auto current = read32(address);
    value = current & ~value;
  } else if (p == 0x1000E010u) {
    if (!valid(address, 4)) return;
    const auto current = read32(address);
    const auto status = (current & 0xFFFFu) & ~(value & 0xFFFFu);
    const auto mask = ((current >> 16) ^ (value >> 16)) & 0xFFFFu;
    value = status | (mask << 16);
  } else if (p == 0x1000F010u) {
    if (!valid(address, 4)) return;
    value = read32(address) ^ value; // EE INTC mask bits toggle on write.
  } else if (p == 0x10001800u) {
    value &= 0xFFFFu;
    timer3_target_future_ = value >= (read32(0x10001820u) & 0xFFFFu);
  } else if (p == 0x10001810u) {
    const auto current = read32(address);
    value = ((current & 0xC00u) & ~(value & 0xC00u)) | (value & 0x3FFu);
  } else if (p == 0x10001820u) {
    value &= 0xFFFFu;
    timer3_target_future_ = value <= (read32(0x10001800u) & 0xFFFFu);
  }
  write16(address, static_cast<std::uint16_t>(value));
  write16(address + 2, static_cast<std::uint16_t>(value >> 16));
}

void Memory::write64(std::uint32_t address, std::uint64_t value) {
  write32(address, static_cast<std::uint32_t>(value));
  write32(address + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint32_t Memory::cycles_until_next_event() const {
  const auto raw_ee = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(hw_[offset]) |
        (static_cast<std::uint32_t>(hw_[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(hw_[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(hw_[offset + 3]) << 24);
  };
  const auto raw_iop = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(iop_hw_[offset]) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 3]) << 24);
  };

  // Treat each IOP clock edge as a boundary for now. This is intentionally
  // conservative until Timer 5 and IOP execution expose their exact deadlines.
  std::uint32_t distance = 8u - iop_cycle_remainder_;
  distance = std::min(distance, hblank_cycles_remaining_);
  distance = std::min(distance, video_cycles_remaining_);
  for (const auto remaining : spu2_dma_cycles_remaining_) {
    if (remaining != 0u) distance = std::min(distance, remaining);
  }
  if (sif0_cycles_remaining_ != 0u)
    distance = std::min(distance, sif0_cycles_remaining_);
  if (sif1_cycles_remaining_ != 0u)
    distance = std::min(distance, sif1_cycles_remaining_);
  if (gif_cycles_remaining_ != 0u)
    distance = std::min(distance, gif_cycles_remaining_);
  if (vif1_cycles_remaining_ != 0u)
    distance = std::min(distance, vif1_cycles_remaining_);

  // SIF starts are discovered by advance(), so an armed pair with no scheduled
  // countdown can transition on the very next call.
  const bool sif1_armed = sif1_cycles_remaining_ == 0u &&
      (raw_ee(0xC400u) & 0x100u) != 0u &&
      (raw_iop(0x0538u) & 0x01000000u) != 0u;
  const bool sif0_armed = sif0_cycles_remaining_ == 0u &&
      (raw_ee(0xC000u) & 0x100u) != 0u &&
      (raw_iop(0x0528u) & 0x01000000u) != 0u;
  const auto gif_chcr = raw_ee(0xA000u);
  const bool gif_armed = gif_cycles_remaining_ == 0u &&
      (gif_chcr & 0x100u) != 0u && (gif_chcr & 0xCu) == 0u &&
      (raw_ee(0xA020u) & 0xFFFFu) != 0u;
  const auto vif1_chcr = raw_ee(0x9000u);
  const bool vif1_armed = vif1_cycles_remaining_ == 0u &&
      (vif1_chcr & 0x100u) != 0u && (vif1_chcr & 0xCu) == 0x4u;
  return sif0_armed || sif1_armed || gif_armed || vif1_armed
      ? 1u : distance;
}

bool Memory::build_vif1_chain(std::vector<std::uint8_t>* packet,
                              std::uint32_t& final_tadr,
                              std::uint32_t& final_madr,
                              std::uint32_t& total_qwc) const {
  auto tadr = read32(0x10009030u) & 0x0FFFFFF0u;
  const auto chcr = read32(0x10009000u);
  std::array<std::uint32_t, 2> return_stack{};
  unsigned return_depth = 0;
  total_qwc = 0;
  final_tadr = tadr;
  final_madr = 0;
  const auto append = [&](std::uint32_t source, std::size_t bytes) {
    if (packet == nullptr) return;
    const auto old_size = packet->size();
    packet->resize(old_size + bytes);
    for (std::size_t byte = 0; byte < bytes; ++byte)
      (*packet)[old_size + byte] = read8(source + static_cast<std::uint32_t>(byte));
  };

  for (unsigned tag_index = 0; tag_index < 256u; ++tag_index) {
    if (!valid(tadr, 16u)) return false;
    const auto tag = read64(tadr);
    const auto qwc = static_cast<std::uint32_t>(tag & 0xFFFFu);
    const auto id = static_cast<unsigned>((tag >> 28) & 7u);
    const auto address = static_cast<std::uint32_t>(tag >> 32) & 0x7FFFFFF0u;
    const bool inline_data = id == 1u || id == 2u || id >= 5u;
    const auto source = inline_data ? tadr + 16u : address;
    const auto bytes = static_cast<std::size_t>(qwc) * 16u;
    if (!valid(source, bytes) || total_qwc > UINT32_MAX - qwc) return false;
    if ((chcr & 0x40u) != 0u) append(tadr + 8u, 8u); // TTE tag payload.
    append(source, bytes);
    total_qwc += qwc;
    final_madr = source + static_cast<std::uint32_t>(bytes);

    const bool irq_end = (tag & (1ull << 31)) != 0u &&
                         (chcr & 0x80u) != 0u;
    if (id == 0u || id == 7u || irq_end) {
      final_tadr = inline_data ? source + static_cast<std::uint32_t>(bytes)
                               : tadr + 16u;
      return true;
    }
    if (id == 1u) tadr = source + static_cast<std::uint32_t>(bytes); // CNT
    else if (id == 2u) tadr = address; // NEXT
    else if (id == 3u || id == 4u) tadr += 16u; // REF / REFS
    else if (id == 5u) { // CALL
      if (return_depth >= return_stack.size()) return false;
      return_stack[return_depth++] = source + static_cast<std::uint32_t>(bytes);
      tadr = address;
    } else if (id == 6u) { // RET
      if (return_depth == 0u) return false;
      tadr = return_stack[--return_depth];
    } else {
      return false;
    }
    final_tadr = tadr;
  }
  return false;
}

void Memory::advance(std::uint32_t cycles) {
  const auto raw_ee = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(hw_[offset]) |
        (static_cast<std::uint32_t>(hw_[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(hw_[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(hw_[offset + 3]) << 24);
  };
  const auto store_ee = [&](std::size_t offset, std::uint32_t value) {
    hw_[offset] = static_cast<std::uint8_t>(value);
    hw_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    hw_[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    hw_[offset + 3] = static_cast<std::uint8_t>(value >> 24);
  };
  const auto raw_iop = [&](std::size_t offset) {
    return static_cast<std::uint32_t>(iop_hw_[offset]) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 1]) << 8) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 2]) << 16) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 3]) << 24);
  };
  const auto store_iop = [&](std::size_t offset, std::uint32_t value) {
    iop_hw_[offset] = static_cast<std::uint8_t>(value);
    iop_hw_[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    iop_hw_[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    iop_hw_[offset + 3] = static_cast<std::uint8_t>(value >> 24);
  };

  // HBlank is Timer 3's external clock source. Preserve the fractional NTSC
  // scanline phase so counter reads remain deterministic over long runs.
  auto hblank_cycles = cycles;
  while (hblank_cycles >= hblank_cycles_remaining_) {
    hblank_cycles -= hblank_cycles_remaining_;
    hblank_cycles_remaining_ = kNtscScanlineCycles;
    hblank_cycle_remainder_ += kNtscScanlineRemainder;
    if (hblank_cycle_remainder_ >= kNtscScanlineDivisor) {
      hblank_cycle_remainder_ -= kNtscScanlineDivisor;
      ++hblank_cycles_remaining_;
    }

    auto mode = raw_ee(0x1810u);
    if ((mode & 0x83u) == 0x83u && (mode & 0x04u) == 0u) {
      const auto old_count = raw_ee(0x1800u) & 0xFFFFu;
      const auto target = raw_ee(0x1820u) & 0xFFFFu;
      auto new_count = (old_count + 1u) & 0xFFFFu;
      const bool overflowed = old_count == 0xFFFFu;
      if (overflowed) {
        timer3_target_future_ = false;
        if ((mode & 0x200u) != 0u && (mode & 0x800u) == 0u) {
          mode |= 0x800u;
          store_ee(0xF000u, raw_ee(0xF000u) | (1u << 12));
        }
      }
      const bool target_reached = !timer3_target_future_ &&
          old_count < target && old_count + 1u >= target;
      if (target_reached) {
        if ((mode & 0x100u) != 0u && (mode & 0x400u) == 0u) {
          mode |= 0x400u;
          store_ee(0xF000u, raw_ee(0xF000u) | (1u << 12));
        }
        if ((mode & 0x40u) != 0u && target != 0u)
          new_count = 0u;
        else
          timer3_target_future_ = true;
      }
      store_ee(0x1800u, new_count);
      store_ee(0x1810u, mode);
    }
  }
  hblank_cycles_remaining_ -= hblank_cycles;

  // Drive the video field phase from EE master cycles. VBlank start/end are
  // independent hardware edges observed by both interrupt controllers.
  auto video_cycles = cycles;
  while (video_cycles >= video_cycles_remaining_) {
    video_cycles -= video_cycles_remaining_;
    if (!video_in_vblank_) {
      video_in_vblank_ = true;
      video_cycles_remaining_ = kNtscVblankCycles;
      store_ee(0xF000u, raw_ee(0xF000u) | (1u << 2));
      store_iop(0x0070u, raw_iop(0x0070u) | (1u << 0));
    } else {
      video_in_vblank_ = false;
      video_cycles_remaining_ = kNtscRenderCycles;
      video_field_remainder_ += kNtscFieldRemainder;
      if (video_field_remainder_ >= kNtscFieldDivisor) {
        video_field_remainder_ -= kNtscFieldDivisor;
        ++video_cycles_remaining_;
      }
      store_ee(0xF000u, raw_ee(0xF000u) | (1u << 3));
      store_iop(0x0070u, raw_iop(0x0070u) | (1u << 11));
    }
  }
  video_cycles_remaining_ -= video_cycles;

  // IOP Timer 5 is driven from the same EE master-cycle timeline as DMA.
  // The scheduler executes one IOP cycle per eight EE cycles and preserves
  // the fractional phase across frontend slices.
  const auto total_iop_phase =
      static_cast<std::uint64_t>(iop_cycle_remainder_) + cycles;
  const auto iop_cycles = static_cast<std::uint32_t>(total_iop_phase / 8u);
  iop_cycle_remainder_ = static_cast<std::uint32_t>(total_iop_phase % 8u);
  if (iop_cycles != 0u) {
    auto mode = raw_iop(0x04A4u);
    // Pulsed repeat mode rearms shortly after an interrupt. Advancing to the
    // next IOP cycle is a deterministic approximation of that hardware pulse.
    if ((mode & 0x40u) != 0u && (mode & 0x80u) == 0u)
      mode |= 0x400u;
    if ((mode & 1u) == 0u) { // No gate: count from the selected clock source.
      static constexpr std::uint32_t rates[4] = {1u, 8u, 16u, 256u};
      const auto rate = rates[(mode >> 13) & 3u];
      const auto prescaled =
          static_cast<std::uint64_t>(timer5_prescale_remainder_) + iop_cycles;
      const auto ticks = prescaled / rate;
      timer5_prescale_remainder_ = static_cast<std::uint32_t>(prescaled % rate);
      if (ticks != 0u) {
        const auto old_count = raw_iop(0x04A0u);
        const auto target = raw_iop(0x04A8u);
        const auto expanded = static_cast<std::uint64_t>(old_count) + ticks;
        const bool target_reached = !timer5_target_future_ &&
            old_count < target && expanded >= target;
        const bool overflowed = expanded > UINT32_MAX;
        auto new_count = static_cast<std::uint32_t>(expanded);
        const auto fire_interrupt = [&](std::uint32_t flag) {
          if ((mode & 0x400u) == 0u) return;
          const bool repeat = (mode & 0x40u) != 0u;
          const bool toggle = (mode & 0x80u) != 0u;
          if (repeat || (mode & flag) == 0u)
            store_iop(0x0070u, raw_iop(0x0070u) | (1u << 16));
          if (repeat || toggle) {
            if (toggle)
              mode ^= 0x400u;
            else
              mode &= ~0x400u;
          }
        };
        if (target_reached) {
          if ((mode & 0x10u) != 0u)
            fire_interrupt(0x800u);
          mode |= 0x800u;
          if ((mode & 0x08u) != 0u && target != 0u)
            new_count = static_cast<std::uint32_t>(expanded % target);
          else
            timer5_target_future_ = true;
        }
        if (overflowed) {
          if ((mode & 0x20u) != 0u)
            fire_interrupt(0x1000u);
          mode |= 0x1000u;
          timer5_target_future_ = false;
        }
        store_iop(0x04A0u, new_count);
        store_iop(0x04A4u, mode);
      }
    }
  }

  for (unsigned core = 0; core < 2u; ++core) {
    auto& remaining = spu2_dma_cycles_remaining_[core];
    if (remaining == 0u) continue;
    if (cycles < remaining) {
      remaining -= cycles;
      continue;
    }

    remaining = 0u;
    for (std::uint32_t byte = 0; byte < spu2_dma_bytes_[core]; ++byte) {
      const auto source =
          (spu2_dma_source_[core] + byte) & (kIopRamSize - 1u);
      const auto destination =
          (spu2_dma_target_[core] * 2u + byte) % spu2_ram_.size();
      spu2_ram_[destination] = iop_ram_[source];
    }
    const auto final_source =
        spu2_dma_source_[core] + spu2_dma_bytes_[core];
    const auto final_target =
        (spu2_dma_target_[core] + spu2_dma_bytes_[core] / 2u) & 0xFFFFFu;
    const auto dma_offset = core == 0u ? 0x00C0u : 0x0500u;
    const auto tsa_offset = core == 0u ? 0x01A8u : 0x05A8u;
    const auto attr_offset = core == 0u ? 0x019Au : 0x059Au;
    const auto statx_offset = core == 0u ? 0x0344u : 0x0744u;
    store_iop(dma_offset, final_source & 0x00FFFFFFu);
    store_iop(dma_offset + 4u, 0u);
    store_iop(dma_offset + 8u,
              raw_iop(dma_offset + 8u) & ~0x01000000u);
    spu2_hw_[tsa_offset] = static_cast<std::uint8_t>(final_target >> 16);
    spu2_hw_[tsa_offset + 1u] = 0u;
    spu2_hw_[tsa_offset + 2u] = static_cast<std::uint8_t>(final_target);
    spu2_hw_[tsa_offset + 3u] = static_cast<std::uint8_t>(final_target >> 8);
    auto statx = iop_read16(0x1F900000u + statx_offset);
    statx = static_cast<std::uint16_t>(statx & ~0x0400u);
    if ((iop_read16(0x1F900000u + attr_offset) & 0x0030u) != 0u)
      statx = static_cast<std::uint16_t>(statx | 0x0080u);
    spu2_hw_[statx_offset] = static_cast<std::uint8_t>(statx);
    spu2_hw_[statx_offset + 1u] = static_cast<std::uint8_t>(statx >> 8);

    // DMA4 uses channel 4/status bit 28 in DICR, while DMA7 uses channel
    // zero/status bit 24 in DICR2. Both signal the shared IOP DMA line.
    const auto dicr_offset = core == 0u ? 0x00F4u : 0x0574u;
    const auto status_bit = core == 0u ? 28u : 24u;
    store_iop(dicr_offset, raw_iop(dicr_offset) | (1u << status_bit));
    store_iop(0x0070u, raw_iop(0x0070u) | (1u << 3));
  }

  if (gif_cycles_remaining_ != 0u) {
    if (cycles < gif_cycles_remaining_) {
      gif_cycles_remaining_ -= cycles;
    } else {
      gif_cycles_remaining_ = 0u;
      const auto bytes = static_cast<std::size_t>(gif_dma_qwc_) * 16u;
      std::vector<std::uint8_t> packet(bytes);
      for (std::size_t byte = 0; byte < bytes; ++byte)
        packet[byte] = read8(gif_dma_source_ + static_cast<std::uint32_t>(byte));
      gif_packets_.push_back(std::move(packet));
      store_ee(0xA010u, gif_dma_source_ + static_cast<std::uint32_t>(bytes));
      store_ee(0xA020u, 0u);
      store_ee(0xA000u, raw_ee(0xA000u) & ~0x100u);
      store_ee(0xE010u, raw_ee(0xE010u) | (1u << 2));
      gif_dma_source_ = 0u;
      gif_dma_qwc_ = 0u;
    }
  }

  if (vif1_cycles_remaining_ != 0u) {
    if (cycles < vif1_cycles_remaining_) {
      vif1_cycles_remaining_ -= cycles;
    } else {
      vif1_cycles_remaining_ = 0u;
      std::vector<std::uint8_t> packet;
      std::uint32_t final_tadr = 0;
      std::uint32_t final_madr = 0;
      std::uint32_t total_qwc = 0;
      if (build_vif1_chain(&packet, final_tadr, final_madr, total_qwc)) {
        vif1_packets_.push_back(std::move(packet));
        store_ee(0x9010u, vif1_final_madr_);
        store_ee(0x9020u, 0u);
        store_ee(0x9030u, vif1_final_tadr_);
        store_ee(0x9000u, raw_ee(0x9000u) & ~0x100u);
        store_ee(0xE010u, raw_ee(0xE010u) | (1u << 1));
      }
    }
  }

  if (sif0_cycles_remaining_ != 0u) {
    if (cycles < sif0_cycles_remaining_) {
      sif0_cycles_remaining_ -= cycles;
      return;
    }
    sif0_cycles_remaining_ = 0;

    auto tadr = raw_iop(0x052Cu) & 0x00FFFFFCu;
    std::uint32_t final_iop_madr = 0;
    std::uint32_t final_ee_madr = 0;
    bool iop_complete = false;
    bool ee_complete = false;
    for (unsigned tag_index = 0;
         tag_index < 64u && !iop_complete && !ee_complete; ++tag_index) {
      if (!iop_valid(tadr, 24u)) break;
      const auto iop_tag = iop_read32(tadr);
      const auto source = iop_tag & 0x00FFFFFFu;
      const auto words = iop_read32(tadr + 4u) & 0x000FFFFFu;
      const auto ee_tag = iop_read32(tadr + 8u);
      const auto qwc = ee_tag & 0xFFFFu;
      const auto id = (ee_tag >> 28) & 7u;
      const auto destination = iop_read32(tadr + 12u) & 0x0FFFFFF0u;
      // The reset reply uses an EE CNT tag terminated by IRQ/TIE. Preserve
      // other destination-chain modes as an explicit unimplemented boundary.
      if (id != 1u || qwc == 0u || words > qwc * 4u ||
          !iop_valid(source, words * 4u) || !valid(destination, qwc * 16u))
        break;
      for (std::uint32_t word = 0; word < qwc * 4u; ++word) {
        const auto value = word < words ? iop_read32(source + word * 4u) : 0u;
        write32(destination + word * 4u, value);
      }
      final_iop_madr = source + words * 4u;
      final_ee_madr = destination + qwc * 16u;
      tadr += 16u;
      const bool iop_end = (iop_tag & 0xC0000000u) != 0u;
      const bool ee_end = (ee_tag & 0x80000000u) != 0u &&
                          (raw_ee(0xC000u) & 0x80u) != 0u;
      iop_complete = iop_end;
      ee_complete = ee_end;
    }
    if (iop_complete || ee_complete) {
      store_ee(0xC010u, final_ee_madr);
      store_ee(0xC020u, 0u);
      if (ee_complete) {
        store_ee(0xC000u, raw_ee(0xC000u) & ~0x100u);
        store_ee(0xE010u, raw_ee(0xE010u) | (1u << 5));
      }
      store_ee(0xF240u, raw_ee(0xF240u) & ~(0x20u | 0x2000u));
      store_iop(0x0520u, final_iop_madr);
      store_iop(0x052Cu, tadr);
      if (iop_complete) {
        store_iop(0x0528u, raw_iop(0x0528u) & ~0x01000000u);
        store_iop(0x0574u, raw_iop(0x0574u) | (1u << 26));
        store_iop(0x0070u, raw_iop(0x0070u) | (1u << 3));
      }
    }
  }

  if (sif1_cycles_remaining_ != 0u) {
    if (cycles < sif1_cycles_remaining_) {
      sif1_cycles_remaining_ -= cycles;
      return;
    }
    sif1_cycles_remaining_ = 0;

    auto tadr = raw_ee(0xC430u) & 0x0FFFFFF0u;
    std::uint32_t final_madr = 0;
    std::uint32_t destination = 0;
    std::uint32_t remaining_words = 0;
    bool complete = false;
    for (unsigned tag_index = 0; tag_index < 64u && !complete; ++tag_index) {
      const auto tag = read32(tadr);
      const auto qwc = tag & 0xFFFFu;
      const auto id = (tag >> 28) & 7u;
      const auto source = read32(tadr + 4u) & 0x0FFFFFF0u;
      // EELOAD uses a zero-length NEXT tag to jump between separately built
      // SIF packet chains. No payload is consumed at the NEXT tag itself.
      if (id == 2u && qwc == 0u && valid(source, 16u)) {
        tadr = source;
        continue;
      }
      // The BIOS uses REF/REFE chains. A transfer begins with a four-word SIF
      // tag (IOP destination, word count, attributes) and can continue through
      // following raw REF payloads when it is larger than the first EE tag.
      if ((id != 0u && id != 3u) || qwc == 0u || !valid(source, qwc * 16u))
        break;
      auto payload = source;
      auto available_words = qwc * 4u;
      if (remaining_words == 0u) {
        destination = read32(source) & 0x00FFFFFFu;
        remaining_words = read32(source + 4u) & 0x000FFFFFu;
        payload += 16u;
        available_words -= 4u;
      }
      const auto words = std::min(remaining_words, available_words);
      if (!iop_valid(destination, words * 4u) || !valid(payload, words * 4u))
        break;
      for (std::uint32_t word = 0; word < words; ++word)
        iop_write32(destination + word * 4u,
                    read32(payload + word * 4u));
      destination += words * 4u;
      remaining_words -= words;
      final_madr = source + qwc * 16u;
      store_iop(0x0530u, destination);
      tadr += 16u;
      const bool end = id == 0u || ((tag & 0x80000000u) != 0u &&
                                   (raw_ee(0xC400u) & 0x80u) != 0u);
      if (end) complete = remaining_words == 0u;
    }
    if (complete) {
      store_ee(0xC410u, final_madr);
      store_ee(0xC420u, 0u);
      store_ee(0xC430u, tadr);
      store_ee(0xC400u, raw_ee(0xC400u) & ~0x100u);
      store_ee(0xE010u, raw_ee(0xE010u) | (1u << 6));
      store_ee(0xF240u, raw_ee(0xF240u) & ~0x4000u);
      store_iop(0x0538u, raw_iop(0x0538u) & ~0x01000000u);
      store_iop(0x0574u, raw_iop(0x0574u) | (1u << 27));
      store_iop(0x0070u, raw_iop(0x0070u) | (1u << 3));
    }
  }

  if (sif1_cycles_remaining_ == 0u &&
      (raw_ee(0xC400u) & 0x100u) != 0u &&
      (raw_iop(0x0538u) & 0x01000000u) != 0u) {
    auto tadr = raw_ee(0xC430u) & 0x0FFFFFF0u;
    std::uint32_t total_qwc = 0;
    bool complete_chain = false;
    for (unsigned tag_index = 0; tag_index < 64u && !complete_chain;
         ++tag_index) {
      if (!valid(tadr, 16u)) break;
      const auto tag = read32(tadr);
      const auto qwc = tag & 0xFFFFu;
      const auto id = (tag >> 28) & 7u;
      const auto source = read32(tadr + 4u) & 0x0FFFFFF0u;
      if (id == 2u && qwc == 0u && valid(source, 16u)) {
        tadr = source;
        continue;
      }
      if ((id != 0u && id != 3u) || qwc == 0u ||
          total_qwc > (UINT32_MAX / 8u) - qwc)
        break;
      total_qwc += qwc;
      tadr += 16u;
      complete_chain = id == 0u || ((tag & 0x80000000u) != 0u &&
                                   (raw_ee(0xC400u) & 0x80u) != 0u);
    }
    if (complete_chain) {
      sif1_cycles_remaining_ = total_qwc * 8u;
      store_ee(0xF240u, raw_ee(0xF240u) | 0x4000u);
    }
  }

  if (sif0_cycles_remaining_ == 0u &&
      (raw_ee(0xC000u) & 0x100u) != 0u &&
      (raw_iop(0x0528u) & 0x01000000u) != 0u) {
    auto tadr = raw_iop(0x052Cu) & 0x00FFFFFCu;
    std::uint32_t total_words = 0;
    bool reached_boundary = false;
    for (unsigned tag_index = 0; tag_index < 64u && !reached_boundary;
         ++tag_index) {
      if (!iop_valid(tadr, 24u)) break;
      const auto iop_tag = iop_read32(tadr);
      const auto words = iop_read32(tadr + 4u) & 0x000FFFFFu;
      const auto ee_tag = iop_read32(tadr + 8u);
      const auto qwc = ee_tag & 0xFFFFu;
      const auto id = (ee_tag >> 28) & 7u;
      if (id != 1u || qwc == 0u || words > qwc * 4u ||
          total_words > (UINT32_MAX / 8u) - words)
        break;
      total_words += words;
      tadr += 16u;
      const bool iop_end = (iop_tag & 0xC0000000u) != 0u;
      const bool ee_end = (ee_tag & 0x80000000u) != 0u &&
                          (raw_ee(0xC000u) & 0x80u) != 0u;
      // SIF0's IOP source chain and EE destination chain terminate
      // independently. A burst ends when either side reaches its tag; the
      // other channel remains armed for a subsequent burst.
      reached_boundary = iop_end || ee_end;
    }
    if (reached_boundary) {
      sif0_cycles_remaining_ = (total_words == 0u ? 1u : total_words) * 8u;
      store_ee(0xF240u, raw_ee(0xF240u) | 0x20u | 0x2000u);
    }
  }

  if (gif_cycles_remaining_ == 0u) {
    const auto chcr = raw_ee(0xA000u);
    const auto qwc = raw_ee(0xA020u) & 0xFFFFu;
    const auto source = raw_ee(0xA010u) & 0x0FFFFFF0u;
    if ((chcr & 0x100u) != 0u && (chcr & 0xCu) == 0u && qwc != 0u &&
        valid(source, static_cast<std::size_t>(qwc) * 16u)) {
      gif_dma_source_ = source;
      gif_dma_qwc_ = qwc;
      gif_cycles_remaining_ = qwc * 8u;
    }
  }

  if (vif1_cycles_remaining_ == 0u) {
    const auto chcr = raw_ee(0x9000u);
    if ((chcr & 0x100u) != 0u && (chcr & 0xCu) == 0x4u) {
      std::uint32_t total_qwc = 0;
      if (build_vif1_chain(nullptr, vif1_final_tadr_, vif1_final_madr_,
                           total_qwc))
        vif1_cycles_remaining_ = (total_qwc == 0u ? 1u : total_qwc) * 8u;
    }
  }
}

bool Memory::pop_gif_packet(std::vector<std::uint8_t>& packet) {
  if (gif_packets_.empty()) return false;
  packet = std::move(gif_packets_.front());
  gif_packets_.pop_front();
  return true;
}

bool Memory::pop_vif1_packet(std::vector<std::uint8_t>& packet) {
  if (vif1_packets_.empty()) return false;
  packet = std::move(vif1_packets_.front());
  vif1_packets_.pop_front();
  return true;
}

std::uint32_t Memory::ee_interrupt_lines() const {
  const auto intc_status = read32(0x1000F000u);
  const auto intc_mask = read32(0x1000F010u);
  const auto dmac = read32(0x1000E010u);
  std::uint32_t lines = 0;
  if ((intc_status & intc_mask) != 0u) lines |= 0x400u;
  if (((dmac & 0xFFFFu) & (dmac >> 16)) != 0u) lines |= 0x800u;
  return lines;
}

bool Memory::iop_interrupt_pending() const {
  return iop_read32(0x1F801078u) != 0u &&
      (iop_read32(0x1F801070u) & iop_read32(0x1F801074u)) != 0u;
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
  if (p >= 0x1F900000u && p < 0x1F900800u)
    return size <= 0x1F900800u - p;
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
  // CDVD reset state with no disc inserted. The IOP BIOS probes N-READY
  // before issuing any command; MECHA_INIT and DEV9CON accompany READY.
  if (p >= 0x1F402000u && p < 0x1F402100u) {
    switch (p & 0xFFu) {
    case 0x05u: return 0x4Cu; // N-READY: ready | mechacon | DEV9 connected
    case 0x0Au: return 0x01u; // STATUS: tray open
    case 0x0Bu: return 0x01u; // sticky STATUS
    case 0x17u: return cdvd_sready_;
    case 0x18u: { // S-DATAOUT
      if ((cdvd_sready_ & 0x40u) != 0u ||
          cdvd_scmd_result_pos_ >= cdvd_scmd_result_count_)
        return 0u;
      const auto value = cdvd_scmd_result_[cdvd_scmd_result_pos_++];
      if (cdvd_scmd_result_pos_ >= cdvd_scmd_result_count_)
        cdvd_sready_ |= 0x40u;
      return value;
    }
    default: return 0u;
    }
  }
  if (p >= 0x1F800000u && p < 0x1F801000u)
    return iop_scratch_[p - 0x1F800000u];
  if (p >= 0x1F900000u && p < 0x1F900800u)
    return spu2_hw_[p - 0x1F900000u];
  if (p == 0x1F808264u)
    return 0xFFu; // SIO2 FIFO: disconnected device response.
  if (p >= 0x1FFE0130u && p < 0x1FFE0134u)
    return static_cast<std::uint8_t>(iop_cache_control_ >> ((p & 3u) * 8u));
  if (p >= kIopHwBase && p < kIopHwBase + kIopHwSize)
    return iop_hw_[p - kIopHwBase];
  return 0;
}

std::uint16_t Memory::iop_read16(std::uint32_t address) const {
  const auto p = address & 0x1FFFFFFFu;
  if (p == 0x1F8014A4u) {
    const auto offset = static_cast<std::size_t>(p - 0x1F801000u);
    const auto value = static_cast<std::uint16_t>(iop_hw_[offset]) |
        (static_cast<std::uint16_t>(iop_hw_[offset + 1u]) << 8);
    // Reading mode acknowledges the target/overflow flags and arms the next
    // interrupt. Bit 10 is the timer's internal interrupt-enable latch.
    iop_hw_[offset + 1u] =
        static_cast<std::uint8_t>((iop_hw_[offset + 1u] & 0xE7u) | 0x04u);
    return value;
  }
  return static_cast<std::uint16_t>(iop_read8(address)) |
      (static_cast<std::uint16_t>(iop_read8(address + 1u)) << 8);
}

std::uint32_t Memory::iop_read32(std::uint32_t address) const {
  const auto p = address & 0x1FFFFFFFu;
  if (p == 0x1F8014A4u) {
    const auto offset = static_cast<std::size_t>(p - 0x1F801000u);
    const auto value = static_cast<std::uint32_t>(iop_hw_[offset]) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 1u]) << 8) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 2u]) << 16) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 3u]) << 24);
    iop_hw_[offset + 1u] =
        static_cast<std::uint8_t>((iop_hw_[offset + 1u] & 0xE7u) | 0x04u);
    return value;
  }
  return static_cast<std::uint32_t>(iop_read16(address)) |
      (static_cast<std::uint32_t>(iop_read16(address + 2u)) << 16);
}

void Memory::iop_write8(std::uint32_t address, std::uint8_t value) {
  const auto p = address & 0x1FFFFFFFu;
  if (p < kIopWindowSize) {
    iop_ram_[p & (kIopRamSize - 1u)] = value;
  } else if (p >= 0x1F402000u && p < 0x1F402100u) {
    const auto reg = p & 0xFFu;
    if (reg == 0x17u) { // S-DATAIN
      if (cdvd_scmd_param_count_ < cdvd_scmd_params_.size())
        cdvd_scmd_params_[cdvd_scmd_param_count_++] = value;
    } else if (reg == 0x16u) { // S-COMMAND
      const auto set_result = [&](std::uint8_t count) {
        cdvd_scmd_result_count_ = count;
        cdvd_scmd_result_pos_ = 0;
        cdvd_sready_ &= ~0x40u;
      };
      cdvd_scmd_result_.fill(0);
      switch (value) {
      case 0x03u: // Mechacon command.
        if (cdvd_scmd_param_count_ != 0u && cdvd_scmd_params_[0] == 0x00u) {
          // GetMechaVersion returns a little-endian RR.MM.mm.TT word. This
          // retail-compatible baseline matches the response expected by the
          // BIOS CDVD module before it starts its higher-level RPC service.
          cdvd_scmd_result_[0] = 0x03u;
          cdvd_scmd_result_[1] = 0x06u;
          cdvd_scmd_result_[2] = 0x02u;
          cdvd_scmd_result_[3] = 0x00u;
          set_result(4u);
        } else {
          cdvd_scmd_result_[0] = 0x81u;
          set_result(1u);
        }
        break;
      case 0x15u: // ForbidDVDP
        // Retail OSDSYS uses this command to disable DVD-Video playback while
        // entering the browser. A normal PS2 returns 5; reporting the generic
        // unsupported-command value makes CDVDFSV retry the RPC indefinitely.
        cdvd_scmd_result_[0] = 0x05u;
        set_result(1u);
        break;
      case 0x22u: // ReadWakeUpTime
        // With no wake-up alarm programmed the drive returns a successful
        // ten-byte all-zero record. OSDSYS polls this during browser startup.
        set_result(10u);
        break;
      case 0x24u: // RCBypassCtrl
        // The retail BIOS initializes the remote-control bypass even when no
        // receiver is attached. A zero result acknowledges the requested mode.
        set_result(1u);
        break;
      case 0x36u: { // ReadRegionParams
        // The 02.00E retail BIOS asks for the optical-drive region block while
        // bringing up cdvdman. Return the complete 15-byte response; a generic
        // one-byte error makes the guest repeatedly reissue this command.
        constexpr std::array<std::uint8_t, 8> region = {
            'E', 'E', 'e', 'n', 'g', 'E', 'E', 0u};
        cdvd_scmd_result_[1] = 0x08u;
        std::copy(region.begin(), region.end(), cdvd_scmd_result_.begin() + 3u);
        set_result(15u);
        break;
      }
      case 0x40u: // OpenConfig(read/write, area, block count)
        if (cdvd_scmd_param_count_ >= 3u) {
          cdvd_config_rw_ = cdvd_scmd_params_[0];
          cdvd_config_offset_ = cdvd_scmd_params_[1];
          cdvd_config_blocks_ = cdvd_scmd_params_[2];
          cdvd_config_index_ = 0;
        }
        set_result(1u);
        break;
      case 0x41u: // ReadConfig
        if (cdvd_config_rw_ != 0u)
          cdvd_scmd_result_[0] = 0x80u;
        if (cdvd_config_index_ < cdvd_config_blocks_)
          ++cdvd_config_index_;
        set_result(16u);
        break;
      case 0x42u: // WriteConfig
        if (cdvd_config_rw_ != 1u)
          cdvd_scmd_result_[0] = 0x80u;
        if (cdvd_config_index_ < cdvd_config_blocks_)
          ++cdvd_config_index_;
        set_result(1u);
        break;
      case 0x43u: // CloseConfig
        cdvd_config_rw_ = cdvd_config_offset_ = 0u;
        cdvd_config_blocks_ = cdvd_config_index_ = 0u;
        set_result(1u);
        break;
      default:
        cdvd_scmd_result_[0] = 0x80u;
        set_result(1u);
        break;
      }
      cdvd_scmd_param_count_ = 0;
    }
  } else if (p >= 0x1F800000u && p < 0x1F801000u) {
    iop_scratch_[p - 0x1F800000u] = value;
  } else if (p >= 0x1F900000u && p < 0x1F900800u) {
    spu2_hw_[p - 0x1F900000u] = value;
  } else if (p >= 0x1FFE0130u && p < 0x1FFE0134u) {
    const unsigned shift = (p & 3u) * 8u;
    iop_cache_control_ = (iop_cache_control_ & ~(0xFFu << shift)) |
        (static_cast<std::uint32_t>(value) << shift);
  } else if (p >= kIopHwBase && p < kIopHwBase + kIopHwSize) {
    iop_hw_[p - kIopHwBase] = value;
  }
}

void Memory::iop_write16(std::uint32_t address, std::uint16_t value) {
  const auto p = address & 0x1FFFFFFFu;
  if (p == 0x1F8014A4u) {
    const auto offset = static_cast<std::size_t>(p - 0x1F801000u);
    const auto current = static_cast<std::uint16_t>(iop_hw_[offset]) |
        (static_cast<std::uint16_t>(iop_hw_[offset + 1u]) << 8);
    auto flags = static_cast<std::uint16_t>(current & 0x1C00u);
    const bool active_event = (current & 0x1800u) != 0u &&
        (current & 0x30u) != 0u;
    if (!active_event)
      flags = 0x400u;
    value = static_cast<std::uint16_t>((value & 0x63FFu) | flags);
    iop_hw_[0x04A0u] = iop_hw_[0x04A1u] = 0u;
    iop_hw_[0x04A2u] = iop_hw_[0x04A3u] = 0u;
    timer5_prescale_remainder_ = 0u;
    timer5_target_future_ = false;
  }
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
  if (p == 0x1F801070u) {
    value = iop_read32(address) & value;
  } else if (p == 0x1F8010F4u) {
    const auto current = iop_read32(address);
    const auto flags = (current & 0x7F000000u) & ~(value & 0x7F000000u);
    value = flags | (value & 0x00FFFFFFu);
  } else if (p == 0x1F801574u) {
    const auto current = iop_read32(address);
    const auto flags = (current & 0x7F000000u) & ~(value & 0x7F000000u);
    value = flags | (value & 0x00FFFFFFu);
  } else if (p == 0x1F801450u && (value & 2u) != 0u) {
    hw_[0xF000u] |= 2u;
  } else if (p == 0x1F8014A4u) {
    const auto offset = static_cast<std::size_t>(p - 0x1F801000u);
    const auto current = static_cast<std::uint32_t>(iop_hw_[offset]) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 1u]) << 8) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 2u]) << 16) |
        (static_cast<std::uint32_t>(iop_hw_[offset + 3u]) << 24);
    auto flags = current & 0x1C00u;
    const bool active_event = (current & 0x1800u) != 0u &&
        (current & 0x30u) != 0u;
    if (!active_event)
      flags = 0x400u;
    value = (value & 0x63FFu) | flags;
    iop_hw_[0x04A0u] = iop_hw_[0x04A1u] = 0u;
    iop_hw_[0x04A2u] = iop_hw_[0x04A3u] = 0u;
    timer5_prescale_remainder_ = 0u;
    timer5_target_future_ = false;
  } else if (p == 0x1F8014A0u) {
    timer5_target_future_ = value > iop_read32(0x1F8014A8u);
  } else if (p == 0x1F8014A8u) {
    timer5_target_future_ = value <= iop_read32(0x1F8014A0u);
  } else if (p == 0x1F808268u && (value & 1u) != 0u) {
    // Starting a SIO2 PIO transfer makes the disconnected-device response
    // available and raises the hardware's IOP interrupt line 17.
    iop_hw_[0x726Cu] = 0x00u;
    iop_hw_[0x726Du] = 0xD1u;
    iop_hw_[0x726Eu] = 0x01u;
    iop_hw_[0x0072u] |= 0x02u;
  } else if ((p == 0x1F8010C8u || p == 0x1F801508u) &&
             (value & 0x01000000u) != 0u) {
    // SPU2 cores 0 and 1 use IOP DMA channels 4 and 7 respectively. BCR
    // describes 32-bit words while TSA is measured in 16-bit sound-RAM words.
    const unsigned core = p == 0x1F8010C8u ? 0u : 1u;
    const auto dma_base = core == 0u ? 0x1F8010C0u : 0x1F801500u;
    const auto tsa_base = core == 0u ? 0x1F9001A8u : 0x1F9005A8u;
    const auto bcr = iop_read32(dma_base + 4u);
    const auto blocks = bcr >> 16;
    const auto words = bcr & 0xFFFFu;
    const auto transfer_words = static_cast<std::uint64_t>(blocks) * words;
    const auto transfer_bytes = transfer_words * 4u;
    if ((value & 0x00000201u) == 0x00000201u && transfer_bytes != 0u &&
        transfer_bytes <= UINT32_MAX) {
      spu2_dma_source_[core] = iop_read32(dma_base) & 0x00FFFFFFu;
      const auto tsa_high = iop_read16(tsa_base) & 0xFu;
      const auto tsa_low = iop_read16(tsa_base + 2u);
      spu2_dma_target_[core] = (tsa_high << 16) | tsa_low;
      spu2_dma_bytes_[core] = static_cast<std::uint32_t>(transfer_bytes);
      // PCSX2's documented SPU2 timing uses 24 IOP cycles per 16-bit word;
      // one IOP cycle corresponds to eight EE master cycles here.
      const auto duration = transfer_bytes * 96u;
      spu2_dma_cycles_remaining_[core] = duration > UINT32_MAX
          ? UINT32_MAX : static_cast<std::uint32_t>(duration);
      const auto statx_address = core == 0u ? 0x1F900344u : 0x1F900744u;
      const auto statx = static_cast<std::uint16_t>(
          (iop_read16(statx_address) & ~0x0080u) | 0x0400u);
      iop_write16(statx_address, statx);
    }
  }
  iop_write8(address, static_cast<std::uint8_t>(value));
  iop_write8(address + 1u, static_cast<std::uint8_t>(value >> 8));
  iop_write8(address + 2u, static_cast<std::uint8_t>(value >> 16));
  iop_write8(address + 3u, static_cast<std::uint8_t>(value >> 24));
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
