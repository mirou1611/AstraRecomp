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
  vertex_count_ = 0;
  pending_.clear();
  packets_submitted_ = 0;
  packets_rejected_ = 0;
  sprites_emitted_ = 0;
  packed_tags_ = 0;
  reglist_tags_ = 0;
  image_tags_ = 0;
  image_bytes_ = 0;
  first_unsupported_tag_ = 0;
}

bool Gif::submit(const std::uint8_t* data, std::size_t size) {
  ++packets_submitted_;
  pending_.insert(pending_.end(), data, data + size);
  std::size_t cursor = 0;
  while (cursor + 16u <= pending_.size()) {
    const auto tag_start = cursor;
    const auto tag = load64(pending_.data() + cursor);
    const auto registers = load64(pending_.data() + cursor + 8u);
    const auto loops = static_cast<unsigned>(tag & 0x7FFFu);
    const auto format = static_cast<unsigned>((tag >> 58) & 3u);
    auto register_count = static_cast<unsigned>((tag >> 60) & 0xFu);
    if (register_count == 0u) register_count = 16u;
    std::size_t payload_bytes = 0;
    if (format == 0u)
      payload_bytes = static_cast<std::size_t>(loops) * register_count * 16u;
    else if (format == 1u)
      payload_bytes = ((static_cast<std::size_t>(loops) * register_count + 1u) /
                       2u) * 16u;
    else
      payload_bytes = static_cast<std::size_t>(loops) * 16u;
    if (pending_.size() - tag_start < 16u + payload_bytes) break;
    cursor += 16u;
    if ((tag & (1ull << 46)) != 0u)
      set_prim((tag >> 47) & 0x7FFu);

    if (format == 0u) {
      ++packed_tags_;
      // Packed mode consumes one qword per register. A+D carries the GS
      // address in the high half; ordinary descriptors directly select one
      // of the common packed registers.
      for (unsigned loop = 0; loop < loops; ++loop) {
        for (unsigned reg = 0; reg < register_count; ++reg) {
          const auto descriptor = static_cast<unsigned>(
              (registers >> ((reg & 15u) * 4u)) & 0xFu);
          const auto value = load64(pending_.data() + cursor);
          const auto upper = load64(pending_.data() + cursor + 8u);
          if (descriptor == 0xEu)
            write_register(static_cast<std::uint8_t>(upper), value);
          else if (descriptor != 0xFu)
            write_register(static_cast<std::uint8_t>(descriptor), value);
          cursor += 16u;
        }
      }
    } else if (format == 1u) {
      ++reglist_tags_;
      // REGLIST packs two 64-bit GS values into each qword and pads an odd
      // value count to the next qword. Register descriptors are direct.
      for (unsigned loop = 0; loop < loops; ++loop) {
        for (unsigned reg = 0; reg < register_count; ++reg) {
          const auto descriptor = static_cast<unsigned>(
              (registers >> ((reg & 15u) * 4u)) & 0xFu);
          if (descriptor != 0xEu && descriptor != 0xFu)
            write_register(static_cast<std::uint8_t>(descriptor),
                           load64(pending_.data() + cursor));
          cursor += 8u;
        }
      }
      cursor = (cursor + 15u) & ~std::size_t{15u};
    } else {
      ++image_tags_;
      // IMAGE/IMAGE2 qwords contain raw transfer data, not register values.
      // Traverse them so later tags in the same DMA packet are still parsed.
      cursor += payload_bytes;
      image_bytes_ += payload_bytes;
    }
  }
  if (cursor != 0u)
    pending_.erase(pending_.begin(), pending_.begin() +
                   static_cast<std::ptrdiff_t>(cursor));
  return true;
}

void Gif::set_prim(std::uint64_t value) {
  if (prim_ != value) {
    vertex_count_ = 0;
    have_first_xyz2_ = false;
  }
  prim_ = value;
}

void Gif::write_register(std::uint8_t address, std::uint64_t value) {
  switch (address) {
  case 0x00: set_prim(value); break;
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
  const auto primitive = static_cast<unsigned>(prim_ & 7u);
  const auto context = static_cast<unsigned>((prim_ >> 9) & 1u);
  const auto make_vertex = [&](std::uint64_t xyz) {
    return GsVertex{
        scaled_coordinate(xyz, xyoffset_[context], 0u, 0u),
        scaled_coordinate(xyz, xyoffset_[context], 16u, 32u),
        static_cast<std::uint32_t>(xyz >> 32),
        static_cast<std::uint32_t>(rgbaq_)};
  };

  if (primitive == 0u) {
    gs_.point(make_vertex(value));
    return;
  }
  if (primitive == 1u || primitive == 2u) {
    const auto vertex = make_vertex(value);
    if (vertex_count_ != 0u) {
      gs_.line(vertices_[0], vertex);
      if (primitive == 1u) vertex_count_ = 0u;
      else vertices_[0] = vertex;
    } else {
      vertices_[0] = vertex;
      vertex_count_ = 1u;
    }
    return;
  }
  if (primitive >= 3u && primitive <= 5u) {
    const auto vertex = make_vertex(value);
    if (vertex_count_ < 2u) {
      vertices_[vertex_count_++] = vertex;
      return;
    }
    gs_.triangle(vertices_[0], vertices_[1], vertex);
    if (primitive == 3u) vertex_count_ = 0u;
    else if (primitive == 4u) {
      vertices_[0] = vertices_[1];
      vertices_[1] = vertex;
    } else {
      vertices_[1] = vertex;
    }
    return;
  }
  if (primitive != 6u) return;
  if (!have_first_xyz2_) {
    first_xyz2_ = value;
    have_first_xyz2_ = true;
    return;
  }

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
