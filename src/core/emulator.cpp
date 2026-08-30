#include "ps2vita/emulator.hpp"

namespace ps2vita {
namespace {

void set_bios_reset_state(CpuState& state) {
  state.cop0[12] = 0x70400004u; // CU0/1/2 + BEV + reset-time ERL.
  state.cop0[16] = 0x00000440u; // R5900 Config reset value.
  state.cop0[15] = 0x00002E20u; // R5900 processor revision.
  state.fcr[0] = 0x00002E30u;   // EE FPU revision.
  state.fcr[31] = 0x01000001u;  // EE FPU control/status reset value.
}

} // namespace

Emulator::Emulator() : memory_(), cpu_(memory_), iop_(memory_) {}

ElfLoadResult Emulator::load_elf(const void* data, std::size_t size) {
  memory_.clear();
  image_ = load_elf32(data, size, memory_);
  ready_ = image_.ok;
  if (ready_) { cpu_.reset(image_.entry); cpu_.set_exception_mode(false); }
  return image_;
}

bool Emulator::load_bios(const void* data, std::size_t size) {
  return memory_.load_bios(data, size);
}

bool Emulator::boot_bios() {
  if (!memory_.has_bios()) return false;
  memory_.clear();
  image_ = {};
  ready_ = true;
  cpu_.reset(0xBFC00000u);
  iop_.reset();
  set_bios_reset_state(cpu_.state());
  cpu_.set_exception_mode(true);
  return true;
}

StopReason Emulator::run_slice(std::uint32_t instructions) {
  if (!ready_) return StopReason::Halted;
  const auto result = cpu_.run(instructions);
  if (!image_.ok) iop_.run((instructions + 7u) / 8u);
  return result;
}

void Emulator::reset() {
  if (image_.ok) { cpu_.reset(image_.entry); cpu_.set_exception_mode(false); }
  else if (memory_.has_bios()) {
    cpu_.reset(0xBFC00000u);
    iop_.reset();
    set_bios_reset_state(cpu_.state());
    cpu_.set_exception_mode(true);
  }
}

} // namespace ps2vita
