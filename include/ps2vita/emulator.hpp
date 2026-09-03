#pragma once

#include "ps2vita/cpu.hpp"
#include "ps2vita/elf_loader.hpp"
#include "ps2vita/gif.hpp"
#include "ps2vita/gs.hpp"
#include "ps2vita/iop_cpu.hpp"

#include <cstddef>

namespace ps2vita {

class Emulator {
public:
  Emulator();
  ElfLoadResult load_elf(const void* data, std::size_t size);
  bool load_bios(const void* data, std::size_t size);
  bool boot_bios();
  StopReason run_slice(std::uint32_t instructions);
  void service_graphics();
  void reset();

  Memory& memory() { return memory_; }
  const Memory& memory() const { return memory_; }
  Cpu& cpu() { return cpu_; }
  const Cpu& cpu() const { return cpu_; }
  IopCpu& iop() { return iop_; }
  const IopCpu& iop() const { return iop_; }
  Gs& gs() { return gs_; }
  const Gs& gs() const { return gs_; }
  const Gif& gif() const { return gif_; }
  const ElfLoadResult& image() const { return image_; }
  bool ready() const { return ready_; }

private:
  Memory memory_;
  Cpu cpu_;
  IopCpu iop_;
  Gs gs_;
  Gif gif_;
  ElfLoadResult image_{};
  std::uint32_t ee_cycles_until_iop_ = 8u;
  bool ready_ = false;
};

} // namespace ps2vita
