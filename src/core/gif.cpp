#include "ps2vita/gif.hpp"

#include <algorithm>
#include <cstring>

namespace ps2vita {
namespace {

std::uint64_t load64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  std::memcpy(&value, data, sizeof(value));
  return value;
}

int scaled_coordinate(std::uint64_t xyz, std::uint64_t offset,
                      unsigned coordinate_shift, unsigned offset_shift) {
  const auto coordinate =
      static_cast<int>((xyz >> coordinate_shift) & 0xFFFFu);
  const auto origin = static_cast<int>((offset >> offset_shift) & 0xFFFFu);
  return (coordinate - origin) / 64; // GS 12.4 coordinates at quarter scale.
}

} // namespace

void Gif::reset() {
  prim_ = 0;
  rgbaq_ = 0x8000000080808080ull;
  xyoffset_[0] = xyoffset_[1] = 0;
  scissor_[0] = scissor_[1] = 0x07FF000007FF0000ull;
  first_xyz2_ = 0;
  have_first_xyz2_ = false;
  packets_submitted_ = 0;
  sprites_emitted_ = 0;
}

bool Gif::submit(const std::uint8_t* data, std::size_t size) {
  ++packets_submitted_;
  std::size_t cursor = 0;
  while (cursor + 16u <= size) {
    const auto tag = load64(data + cursor);
    const auto registers = load64(data + cursor + 8u);
    cursor += 16u;
    const auto loops = static_cast<unsigned>(tag & 0x7FFFu);
    const auto format = static_cast<unsigned>((tag >> 58) & 3u);
    auto register_count = static_cast<unsigned>((tag >> 60) & 0xFu);
    if (register_count == 0u) register_count = 16u;

    // Packed mode consumes one qword for every register in every loop. A+D
    // carries the destination address in the high half's low byte.
    if (format != 0u) return false;
    for (unsigned loop = 0; loop < loops; ++loop) {
      for (unsigned reg = 0; reg < register_count; ++reg) {
        if (cursor + 16u > size) return false;
        const auto descriptor = static_cast<unsigned>(
            (registers >> ((reg & 15u) * 4u)) & 0xFu);
        const auto value = load64(data + cursor);
        const auto upper = load64(data + cursor + 8u);
        if (descriptor == 0xEu)
          write_register(static_cast<std::uint8_t>(upper), value);
        cursor += 16u;
      }
    }
  }
  return cursor <= size;
}

void Gif::write_register(std::uint8_t address, std::uint64_t value) {
  switch (address) {
  case 0x00: prim_ = value; break;
  case 0x01: rgbaq_ = value; break;
  case 0x05: emit_xyz2(value); break;
  case 0x18: xyoffset_[0] = value; break;
  case 0x19: xyoffset_[1] = value; break;
  case 0x40: scissor_[0] = value; break;
  case 0x41: scissor_[1] = value; break;
  default: break;
  }
}

void Gif::emit_xyz2(std::uint64_t value) {
  if ((prim_ & 7u) != 6u) return; // This slice implements GS sprites.
  if (!have_first_xyz2_) {
    first_xyz2_ = value;
    have_first_xyz2_ = true;
    return;
  }

  const auto context = static_cast<unsigned>((prim_ >> 9) & 1u);
  auto x0 = scaled_coordinate(first_xyz2_, xyoffset_[context], 0u, 0u);
  auto y0 = scaled_coordinate(first_xyz2_, xyoffset_[context], 16u, 32u);
  auto x1 = scaled_coordinate(value, xyoffset_[context], 0u, 0u);
  auto y1 = scaled_coordinate(value, xyoffset_[context], 16u, 32u);
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);

  const auto scissor = scissor_[context];
  const auto clip_x0 = static_cast<int>((scissor & 0x7FFu) / 4u);
  const auto clip_x1 = static_cast<int>(((scissor >> 16) & 0x7FFu) / 4u);
  const auto clip_y0 = static_cast<int>(((scissor >> 32) & 0x7FFu) / 4u);
  const auto clip_y1 = static_cast<int>(((scissor >> 48) & 0x7FFu) / 4u);
  x0 = std::max(x0, clip_x0);
  x1 = std::min(x1, clip_x1 + 1);
  y0 = std::max(y0, clip_y0);
  y1 = std::min(y1, clip_y1 + 1);
  const auto z = static_cast<std::uint32_t>(value >> 32);
  const auto color = static_cast<std::uint32_t>(rgbaq_);
  for (auto y = y0; y < y1; ++y)
    for (auto x = x0; x < x1; ++x)
      gs_.point({x, y, z, color});
  ++sprites_emitted_;
  have_first_xyz2_ = false;
}

} // namespace ps2vita
