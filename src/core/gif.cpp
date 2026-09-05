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

Gif::Gif(Gs& gs) : gs_(gs), local_memory_(4u * 1024u * 1024u) {}

void Gif::reset() {
  prim_ = 0;
  rgbaq_ = 0x8000000080808080ull;
  tex0_[0] = tex0_[1] = 0;
  uv_ = 0;
  xyoffset_[0] = xyoffset_[1] = 0;
  scissor_[0] = scissor_[1] = 0x07FF000007FF0000ull;
  bitbltbuf_ = trxpos_ = trxreg_ = trxdir_ = 0;
  first_xyz2_ = 0;
  have_first_xyz2_ = false;
  vertex_count_ = 0;
  pending_.clear();
  packets_submitted_ = 0;
  packets_rejected_ = 0;
  sprites_emitted_ = 0;
  points_emitted_ = lines_emitted_ = triangles_emitted_ = 0;
  packed_tags_ = 0;
  reglist_tags_ = 0;
  image_tags_ = 0;
  image_bytes_ = 0;
  first_image_bitbltbuf_ = first_image_trxpos_ = 0;
  first_image_trxreg_ = first_image_trxdir_ = 0;
  image_records_.clear();
  std::fill(local_memory_.begin(), local_memory_.end(), 0u);
  local_bytes_written_ = 0;
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
    if (format == 0u && loops != 0u && (tag & (1ull << 46)) != 0u)
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
          else if (descriptor == 0x01u) {
            const auto rgba = (value & 0xFFu) |
                (((value >> 32) & 0xFFu) << 8) |
                ((upper & 0xFFu) << 16) |
                (((upper >> 32) & 0xFFu) << 24);
            rgbaq_ = (rgbaq_ & 0xFFFFFFFF00000000ull) | rgba;
          } else if (descriptor == 0x03u) {
            uv_ = (value & 0x3FFFu) | (((value >> 32) & 0x3FFFu) << 16);
          } else if (descriptor == 0x05u || descriptor == 0x0Du) {
            const auto xyz = (value & 0xFFFFu) |
                (((value >> 32) & 0xFFFFu) << 16) |
                ((upper & 0xFFFFFFFFu) << 32);
            emit_xyz2(xyz, descriptor == 0x05u &&
                (upper & (1ull << 47)) == 0u);
          }
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
      if (image_tags_ == 0u) {
        first_image_bitbltbuf_ = bitbltbuf_;
        first_image_trxpos_ = trxpos_;
        first_image_trxreg_ = trxreg_;
        first_image_trxdir_ = trxdir_;
      }
      ++image_tags_;
      GifImageRecord record{};
      record.tag = tag;
      record.bitbltbuf = bitbltbuf_;
      record.trxpos = trxpos_;
      record.trxreg = trxreg_;
      record.trxdir = trxdir_;
      record.bytes = payload_bytes;
      record.first_qword = payload_bytes == 0u ? 0u :
          load64(pending_.data() + cursor);
      record.hash = 1469598103934665603ull;
      for (std::size_t byte = 0; byte < payload_bytes; ++byte) {
        record.hash ^= pending_[cursor + byte];
        record.hash *= 1099511628211ull;
      }
      image_records_.push_back(record);
      write_image(pending_.data() + cursor, payload_bytes);
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

std::uint32_t Gif::read_local32(std::uint32_t byte_address) const {
  std::uint32_t value = 0;
  for (unsigned byte = 0; byte < 4u; ++byte)
    value |= static_cast<std::uint32_t>(
        local_memory_[(byte_address + byte) & 0x3FFFFFu]) << (byte * 8u);
  return value;
}

void Gif::write_image(const std::uint8_t* data, std::size_t size) {
  if ((trxdir_ & 3u) != 0u) return; // This slice models host-to-local only.
  const auto pixel_format = static_cast<unsigned>((bitbltbuf_ >> 56) & 0x3Fu);
  const auto bytes_per_pixel = pixel_format == 0u ? 4u :
                               pixel_format == 2u ? 2u : 0u;
  if (bytes_per_pixel == 0u) return;
  const auto base = static_cast<std::uint32_t>(
      ((bitbltbuf_ >> 32) & 0x3FFFu) * 256u);
  const auto buffer_width = static_cast<unsigned>(
      (bitbltbuf_ >> 48) & 0x3Fu) * 64u;
  const auto destination_x = static_cast<unsigned>((trxpos_ >> 32) & 0x7FFu);
  const auto destination_y = static_cast<unsigned>((trxpos_ >> 48) & 0x7FFu);
  const auto width = static_cast<unsigned>(trxreg_ & 0xFFFu);
  const auto height = static_cast<unsigned>((trxreg_ >> 32) & 0xFFFu);
  if (buffer_width == 0u || width == 0u || height == 0u) return;
  const auto pixels = std::min<std::size_t>(
      size / bytes_per_pixel, static_cast<std::size_t>(width) * height);
  for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
    const auto x = destination_x + static_cast<unsigned>(pixel % width);
    const auto y = destination_y + static_cast<unsigned>(pixel / width);
    const auto destination = base +
        static_cast<std::uint32_t>((y * buffer_width + x) * bytes_per_pixel);
    for (unsigned byte = 0; byte < bytes_per_pixel; ++byte)
      local_memory_[(destination + byte) & 0x3FFFFFu] =
          data[pixel * bytes_per_pixel + byte];
  }
  local_bytes_written_ += pixels * bytes_per_pixel;
}

std::uint32_t Gif::sample_texture(unsigned context, unsigned u, unsigned v,
                                  std::uint32_t vertex_color) const {
  const auto tex0 = tex0_[context & 1u];
  const auto base = static_cast<std::uint32_t>((tex0 & 0x3FFFu) * 256u);
  const auto width = static_cast<unsigned>((tex0 >> 14) & 0x3Fu) * 64u;
  const auto format = static_cast<unsigned>((tex0 >> 20) & 0x3Fu);
  if (width == 0u) return vertex_color;
  std::uint32_t color = 0;
  if (format == 0u) {
    color = read_local32(base + static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(v) * width + u) * 4u));
  } else if (format == 2u) {
    const auto address = base + static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(v) * width + u) * 2u);
    const auto pixel = static_cast<std::uint16_t>(
        local_memory_[address & 0x3FFFFFu] |
        (local_memory_[(address + 1u) & 0x3FFFFFu] << 8u));
    const auto expand = [](std::uint32_t component) {
      return (component << 3u) | (component >> 2u);
    };
    color = expand(pixel & 0x1Fu) |
            (expand((pixel >> 5) & 0x1Fu) << 8u) |
            (expand((pixel >> 10) & 0x1Fu) << 16u) |
            ((pixel & 0x8000u) != 0u ? 0x80000000u : 0u);
  } else {
    return vertex_color;
  }
  const auto texture_function = static_cast<unsigned>((tex0 >> 35) & 3u);
  if (texture_function != 0u) return color; // DECAL/highlight reference path.
  std::uint32_t modulated = 0;
  for (unsigned shift = 0; shift < 32u; shift += 8u) {
    const auto texel = (color >> shift) & 0xFFu;
    const auto vertex = (vertex_color >> shift) & 0xFFu;
    modulated |= std::min(255u, (texel * vertex) >> 7u) << shift;
  }
  return modulated;
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
  case 0x03: uv_ = value; break;
  case 0x06: tex0_[0] = value; break;
  case 0x07: tex0_[1] = value; break;
  case 0x05: emit_xyz2(value); break;
  case 0x18: xyoffset_[0] = value; break;
  case 0x19: xyoffset_[1] = value; break;
  case 0x40: scissor_[0] = value; break;
  case 0x41: scissor_[1] = value; break;
  case 0x50: bitbltbuf_ = value; break;
  case 0x51: trxpos_ = value; break;
  case 0x52: trxreg_ = value; break;
  case 0x53: trxdir_ = value; break;
  default: break;
  }
}

void Gif::emit_xyz2(std::uint64_t value, bool draw) {
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
    if (draw) {
      gs_.point(make_vertex(value));
      ++points_emitted_;
    }
    return;
  }
  if (primitive == 1u || primitive == 2u) {
    const auto vertex = make_vertex(value);
    if (vertex_count_ != 0u) {
      if (draw) {
        gs_.line(vertices_[0], vertex);
        ++lines_emitted_;
      }
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
    if (draw) {
      gs_.triangle(vertices_[0], vertices_[1], vertex);
      ++triangles_emitted_;
    }
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
    first_uv_ = uv_;
    have_first_xyz2_ = true;
    return;
  }

  if (!draw) {
    have_first_xyz2_ = false;
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
  const bool textured_uv = (prim_ & (1u << 4)) != 0u &&
                           (prim_ & (1u << 8)) != 0u;
  const auto u0 = static_cast<int>(first_uv_ & 0x3FFFu) >> 4;
  const auto v0 = static_cast<int>((first_uv_ >> 16) & 0x3FFFu) >> 4;
  const auto u1 = static_cast<int>(uv_ & 0x3FFFu) >> 4;
  const auto v1 = static_cast<int>((uv_ >> 16) & 0x3FFFu) >> 4;
  const auto span_x = std::max(1, x1 - x0);
  const auto span_y = std::max(1, y1 - y0);
  for (auto y = y0; y < y1; ++y) {
    for (auto x = x0; x < x1; ++x) {
      auto pixel_color = color;
      if (textured_uv) {
        const auto u = u0 + (x - x0) * (u1 - u0) / span_x;
        const auto v = v0 + (y - y0) * (v1 - v0) / span_y;
        pixel_color = sample_texture(context,
            static_cast<unsigned>(std::max(0, u)),
            static_cast<unsigned>(std::max(0, v)), color);
      }
      gs_.point({x, y, z, pixel_color});
    }
  }
  ++sprites_emitted_;
  have_first_xyz2_ = false;
}

} // namespace ps2vita
