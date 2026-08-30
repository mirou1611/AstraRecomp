#include "ps2vita/elf_loader.hpp"

#include <cstdint>
#include <cstring>
#include <limits>

namespace ps2vita {
namespace {

#pragma pack(push, 1)
struct Elf32Header {
  unsigned char ident[16];
  std::uint16_t type;
  std::uint16_t machine;
  std::uint32_t version;
  std::uint32_t entry;
  std::uint32_t phoff;
  std::uint32_t shoff;
  std::uint32_t flags;
  std::uint16_t ehsize;
  std::uint16_t phentsize;
  std::uint16_t phnum;
  std::uint16_t shentsize;
  std::uint16_t shnum;
  std::uint16_t shstrndx;
};

struct Elf32ProgramHeader {
  std::uint32_t type;
  std::uint32_t offset;
  std::uint32_t vaddr;
  std::uint32_t paddr;
  std::uint32_t filesz;
  std::uint32_t memsz;
  std::uint32_t flags;
  std::uint32_t align;
};
#pragma pack(pop)

bool range_ok(std::size_t offset, std::size_t length, std::size_t total) {
  return offset <= total && length <= total - offset;
}

} // namespace

ElfLoadResult load_elf32(const void* data, std::size_t size, Memory& memory) {
  ElfLoadResult result{};
  if (!data || size < sizeof(Elf32Header)) {
    result.error = "file too small";
    return result;
  }

  Elf32Header header{};
  std::memcpy(&header, data, sizeof(header));
  if (header.ident[0] != 0x7F || header.ident[1] != 'E' ||
      header.ident[2] != 'L' || header.ident[3] != 'F') {
    result.error = "not an ELF file";
    return result;
  }
  if (header.ident[4] != 1 || header.ident[5] != 1) {
    result.error = "requires ELF32 little-endian";
    return result;
  }
  if (header.machine != 8) {
    result.error = "ELF is not MIPS";
    return result;
  }
  if (header.phentsize < sizeof(Elf32ProgramHeader)) {
    result.error = "invalid program header size";
    return result;
  }
  const std::uint64_t table_size =
      static_cast<std::uint64_t>(header.phentsize) * header.phnum;
  if (table_size > std::numeric_limits<std::size_t>::max() ||
      !range_ok(header.phoff, static_cast<std::size_t>(table_size), size)) {
    result.error = "program headers outside file";
    return result;
  }

  const auto* bytes = static_cast<const std::uint8_t*>(data);
  for (std::uint16_t i = 0; i < header.phnum; ++i) {
    Elf32ProgramHeader ph{};
    std::memcpy(&ph, bytes + header.phoff + i * header.phentsize, sizeof(ph));
    if (ph.type != 1) continue; // PT_LOAD
    if (ph.filesz > ph.memsz || !range_ok(ph.offset, ph.filesz, size)) {
      result.error = "invalid load segment";
      return result;
    }
    if (!memory.valid(ph.vaddr, ph.memsz)) {
      result.error = "segment outside 32 MiB EE RAM";
      return result;
    }
    if (!memory.copy_in(ph.vaddr, bytes + ph.offset, ph.filesz) ||
        !memory.zero(ph.vaddr + ph.filesz, ph.memsz - ph.filesz)) {
      result.error = "failed to map segment";
      return result;
    }
    ++result.segments;
  }
  if (result.segments == 0 || !memory.valid(header.entry, 4)) {
    result.error = result.segments == 0 ? "ELF has no load segments" : "entry outside EE RAM";
    return result;
  }
  result.ok = true;
  result.entry = header.entry;
  return result;
}

} // namespace ps2vita

