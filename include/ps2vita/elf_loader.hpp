#pragma once

#include "ps2vita/memory.hpp"

#include <cstddef>
#include <cstdint>

namespace ps2vita {

struct ElfLoadResult {
  bool ok = false;
  std::uint32_t entry = 0;
  std::uint32_t segments = 0;
  const char* error = nullptr;
};

ElfLoadResult load_elf32(const void* data, std::size_t size, Memory& memory);

} // namespace ps2vita

