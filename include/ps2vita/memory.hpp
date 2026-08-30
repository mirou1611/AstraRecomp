#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <vector>

namespace ps2vita {

class Memory {
public:
  static constexpr std::uint32_t kRamSize = 32u * 1024u * 1024u;
  static constexpr std::uint32_t kBiosBase = 0x1FC00000u;
  static constexpr std::uint32_t kBiosSize = 4u * 1024u * 1024u;
  static constexpr std::uint32_t kScratchBase = 0x70000000u;
  static constexpr std::uint32_t kScratchSize = 16u * 1024u;
  static constexpr std::uint32_t kHwBase = 0x10000000u;
  static constexpr std::uint32_t kHwSize = 64u * 1024u;
  static constexpr std::uint32_t kGsHwBase = 0x12000000u;
  static constexpr std::uint32_t kGsHwSize = 64u * 1024u;
  static constexpr std::uint32_t kVuBase = 0x11000000u;
  static constexpr std::uint32_t kVuSize = 64u * 1024u;
  static constexpr std::uint32_t kVu0MicroBase = 0x11000000u;
  static constexpr std::uint32_t kVu0DataBase = 0x11004000u;
  static constexpr std::uint32_t kVu1MicroBase = 0x11008000u;
  static constexpr std::uint32_t kVu1DataBase = 0x1100C000u;
  static constexpr std::uint32_t kIopBase = 0x1C000000u;
  static constexpr std::uint32_t kIopRamSize = 2u * 1024u * 1024u;
  static constexpr std::uint32_t kIopWindowSize = 8u * 1024u * 1024u;
  static constexpr std::uint32_t kNullBase = kRamSize;
  static constexpr std::uint32_t kNullEnd = 0x10000000u;
  static constexpr std::uint32_t kIopHwBase = 0x1F801000u;
  static constexpr std::uint32_t kIopHwSize = 64u * 1024u;
  // R5900 processor-internal control space used during BIOS cache setup. It
  // lives in KSEG3 but is not a normal TLB-backed memory page.
  static constexpr std::uint32_t kEeInternalBase = 0xFFFE0000u;
  static constexpr std::uint32_t kEeInternalSize = 64u * 1024u;
  static constexpr std::uint32_t kDevBoardBase = 0x1FA00000u;
  static constexpr std::uint32_t kDevBoardSize = 64u * 1024u;
  static constexpr std::uint32_t kDveBase = 0x1A000000u;
  static constexpr std::uint32_t kDveSize = 64u * 1024u;

  Memory();

  void clear();
  bool valid(std::uint32_t address, std::size_t size = 1) const;
  std::uint8_t read8(std::uint32_t address) const;
  std::uint16_t read16(std::uint32_t address) const;
  std::uint32_t read32(std::uint32_t address) const;
  std::uint64_t read64(std::uint32_t address) const;
  void write8(std::uint32_t address, std::uint8_t value);
  void write16(std::uint32_t address, std::uint16_t value);
  void write32(std::uint32_t address, std::uint32_t value);
  void write64(std::uint32_t address, std::uint64_t value);
  // Advances asynchronous hardware models by EE guest cycles.
  void advance(std::uint32_t cycles);
  // Pending external interrupt lines as R5900 Cause.IP bits.
  std::uint32_t ee_interrupt_lines() const;
  bool iop_interrupt_pending() const;
  // IOP has its own physical RAM map but shares ROM and SIF hardware with EE.
  bool iop_valid(std::uint32_t address, std::size_t size = 1) const;
  std::uint8_t iop_read8(std::uint32_t address) const;
  std::uint16_t iop_read16(std::uint32_t address) const;
  std::uint32_t iop_read32(std::uint32_t address) const;
  void iop_write8(std::uint32_t address, std::uint8_t value);
  void iop_write16(std::uint32_t address, std::uint16_t value);
  void iop_write32(std::uint32_t address, std::uint32_t value);
  bool copy_in(std::uint32_t address, const void* source, std::size_t size);
  bool zero(std::uint32_t address, std::size_t size);
  bool load_bios(const void* source, std::size_t size);
  bool has_bios() const { return bios_loaded_; }
  std::uint32_t page_generation(std::uint32_t address) const;
  void clear_tlb();
  void write_tlb(unsigned index, std::uint32_t page_mask, std::uint32_t entry_hi,
                 std::uint32_t entry_lo0, std::uint32_t entry_lo1);
  bool read_tlb(unsigned index, std::uint32_t& page_mask, std::uint32_t& entry_hi,
                std::uint32_t& entry_lo0, std::uint32_t& entry_lo1) const;
  int probe_tlb(std::uint32_t entry_hi) const;

private:
  struct TlbEntry {
    std::uint32_t page_mask = 0;
    std::uint32_t entry_hi = 0;
    std::uint32_t entry_lo0 = 0;
    std::uint32_t entry_lo1 = 0;
    bool written = false;
  };

  std::uint32_t physical(std::uint32_t address) const;
  std::vector<std::uint8_t> ram_;
  std::vector<std::uint8_t> bios_;
  std::vector<std::uint8_t> scratch_;
  std::vector<std::uint8_t> hw_;
  std::vector<std::uint8_t> gs_hw_;
  std::vector<std::uint8_t> vu_mem_;
  std::vector<std::uint8_t> iop_ram_;
  std::array<std::uint8_t, 4096> iop_scratch_{};
  mutable std::vector<std::uint8_t> iop_hw_;
  std::vector<std::uint8_t> ee_internal_;
  mutable std::vector<std::uint8_t> dve_;
  std::array<std::uint16_t, 256> dve_device_regs_{};
  std::uint8_t dve_current_reg_ = 0;
  mutable bool dve_command_executing_ = false;
  bool dve_error_ = false;
  bool bios_loaded_ = false;
  std::array<TlbEntry, 48> tlb_{};
  std::array<std::uint32_t, kRamSize / 4096u> ram_page_generation_{};
  std::uint32_t mapping_generation_ = 1;
  mutable std::uint32_t timer0_count_ = 0;
  mutable unsigned timer0_reads_ = 0;
  mutable unsigned rdram_sdevid_ = 0;
  std::array<std::uint8_t, 16> cdvd_scmd_params_{};
  std::array<std::uint8_t, 16> cdvd_scmd_result_{};
  std::uint8_t cdvd_scmd_param_count_ = 0;
  mutable std::uint8_t cdvd_scmd_result_pos_ = 0;
  std::uint8_t cdvd_scmd_result_count_ = 0;
  mutable std::uint8_t cdvd_sready_ = 0x40;
  std::uint8_t cdvd_config_rw_ = 0;
  std::uint8_t cdvd_config_offset_ = 0;
  std::uint8_t cdvd_config_blocks_ = 0;
  std::uint8_t cdvd_config_index_ = 0;
  std::uint32_t video_cycles_remaining_ = 0;
  std::uint32_t video_field_remainder_ = 0;
  bool video_in_vblank_ = false;
  std::uint32_t iop_cycle_remainder_ = 0;
  std::uint32_t timer5_prescale_remainder_ = 0;
  bool timer5_target_future_ = false;
  std::uint32_t sif0_cycles_remaining_ = 0;
  std::uint32_t sif1_cycles_remaining_ = 0;
  std::uint32_t iop_cache_control_ = 0;
};

} // namespace ps2vita
