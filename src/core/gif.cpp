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

int fixed_coordinate(std::uint64_t xyz, std::uint64_t offset,
                     unsigned coordinate_shift, unsigned offset_shift) {
  const auto coordinate =
      static_cast<int>((xyz >> coordinate_shift) & 0xFFFFu);
  const auto origin = static_cast<int>((offset >> offset_shift) & 0xFFFFu);
  return coordinate - origin;
}

int scaled_coordinate(std::uint64_t xyz, std::uint64_t offset,
                      unsigned coordinate_shift, unsigned offset_shift) {
  return fixed_coordinate(xyz, offset, coordinate_shift, offset_shift) / 64;
}

int ceil_quarter_coordinate(int fixed) {
  // Quarter-grid anchors are native x*4: GS 12.4 fixed x*64. C++ signed
  // division already rounds negative values toward the required ceiling.
  return fixed >= 0 ? (fixed + 63) / 64 : fixed / 64;
}

} // namespace

Gif::Gif(Gs& gs) : gs_(gs), local_memory_(4u * 1024u * 1024u) {}

void Gif::reset() {
  gs_.set_color_target(nullptr, 0u);
  texa_ = 0;
  triangle_records_.clear();
  nondegenerate_triangle_records_.clear();
  sprite_records_.clear();
  sprite_preceding_triangles_.clear();
  recent_triangle_count_ = recent_triangle_cursor_ = 0;
  sprite_framebuffer_capture_.clear();
  sprite_texture_capture_.clear();
  sprite_preceding_texture16_.clear();
  sprite_preceding_texture16_tex0_ = 0;
  sprite_preceding_texture16_width_ = sprite_preceding_texture16_height_ = 0;
  capture_sprite_enabled_ = false;
  prim_ = 0;
  rgbaq_ = 0x8000000080808080ull;
  st_ = 0;
  packed_q_ = 0x3F800000u;
  tex0_[0] = tex0_[1] = 0;
  tex1_[0] = tex1_[1] = 0;
  clamp_[0] = clamp_[1] = 0;
  frame_[0] = frame_[1] = 0;
  test_[0] = test_[1] = 0;
  zbuf_[0] = zbuf_[1] = 0;
  alpha_[0] = alpha_[1] = 0;
  pabe_ = colclamp_ = false;
  uv_ = 0;
  xyoffset_[0] = xyoffset_[1] = 0;
  scissor_[0] = scissor_[1] = 0x07FF000007FF0000ull;
  bitbltbuf_ = trxpos_ = trxreg_ = trxdir_ = 0;
  transfer_pixels_ = 0;
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
    // A new nonempty GIFtag resets the temporary Q, not GS RGBAQ.Q.
    // Do this only when the buffered tag can actually be processed.
    if (loops != 0u) packed_q_ = 0x3F800000u;
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
            rgbaq_ = (std::uint64_t{packed_q_} << 32) | rgba;
          } else if (descriptor == 0x02u) {
            st_ = value;
            packed_q_ = static_cast<std::uint32_t>(upper);
          } else if (descriptor == 0x03u) {
            uv_ = (value & 0x3FFFu) | (((value >> 32) & 0x3FFFu) << 16);
          } else if (descriptor == 0x04u || descriptor == 0x0Cu) {
            // XYZF packs Z in bits 68..91, unlike XYZ2's bits 64..95.
            const auto xyz = (value & 0xFFFFu) |
                (((value >> 32) & 0xFFFFu) << 16) |
                (((upper >> 4) & 0xFFFFFFu) << 32);
            emit_xyz2(xyz, descriptor == 0x04u &&
                (upper & (1ull << 47)) == 0u);
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
  const auto total_pixels = static_cast<std::size_t>(width) * height;
  const auto pixels = std::min<std::size_t>(size / bytes_per_pixel,
      total_pixels - std::min(transfer_pixels_, total_pixels));
  for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
    const auto position = transfer_pixels_ + pixel;
    const auto x = destination_x + static_cast<unsigned>(position % width);
    const auto y = destination_y + static_cast<unsigned>(position / width);
    const auto destination = base +
        static_cast<std::uint32_t>((y * buffer_width + x) * bytes_per_pixel);
    for (unsigned byte = 0; byte < bytes_per_pixel; ++byte)
      local_memory_[(destination + byte) & 0x3FFFFFu] =
          data[pixel * bytes_per_pixel + byte];
  }
  transfer_pixels_ += pixels;
  local_bytes_written_ += pixels * bytes_per_pixel;
}

std::uint32_t Gif::sample_texture(unsigned context, unsigned u, unsigned v,
                                  std::uint32_t vertex_color) const {
  const auto raw_u = u, raw_v = v;
  const auto tex0 = tex0_[context & 1u];
  const auto base = static_cast<std::uint32_t>((tex0 & 0x3FFFu) * 256u);
  const auto width = static_cast<unsigned>((tex0 >> 14) & 0x3Fu) * 64u;
  const auto format = static_cast<unsigned>((tex0 >> 20) & 0x3Fu);
  if (width == 0u) return vertex_color;
  const auto clamp = clamp_[context & 1u];
  const auto address_coordinate = [](unsigned coordinate, unsigned mode,
                                     unsigned size, unsigned minimum, unsigned maximum) {
    switch (mode) {
    case 0: return coordinate & (size - 1u); // REPEAT
    case 1: return std::min(coordinate, size - 1u); // CLAMP
    case 2: return std::min(std::max(coordinate, minimum), maximum); // REGION_CLAMP
    default: return (coordinate & minimum) | maximum; // REGION_REPEAT
    }
  };
  u = address_coordinate(u, clamp & 3u, 1u << ((tex0 >> 26) & 15u),
                         (clamp >> 4) & 0x3FFu, (clamp >> 14) & 0x3FFu);
  v = address_coordinate(v, (clamp >> 2) & 3u, 1u << ((tex0 >> 30) & 15u),
                         (clamp >> 24) & 0x3FFu, (clamp >> 34) & 0x3FFu);
  std::uint32_t color = 0;
  std::uint32_t texel_address = 0, raw_texel = 0;
  if (format == 0u || format == 1u) {
    texel_address = base + static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(v) * width + u) * 4u);
    color = raw_texel = read_local32(texel_address);
    if (format == 1u) {
      color &= 0x00FFFFFFu;
      const auto alpha = (texa_ & 0x8000u) != 0u && color == 0u ? 0u : texa_ & 0xFFu;
      color |= static_cast<std::uint32_t>(alpha) << 24;
    }
  } else if (format == 2u) {
    const auto address = base + static_cast<std::uint32_t>(
        (static_cast<std::uint64_t>(v) * width + u) * 2u);
    texel_address = address;
    const auto pixel = static_cast<std::uint16_t>(
        local_memory_[address & 0x3FFFFFu] |
        (local_memory_[(address + 1u) & 0x3FFFFFu] << 8u));
    raw_texel = pixel;
    const auto expand = [](std::uint32_t component) {
      return component << 3u;
    };
    const auto alpha = (pixel & 0x8000u) != 0u ? (texa_ >> 32u) & 0xFFu :
        ((texa_ & 0x8000u) != 0u && pixel == 0u ? 0u : texa_ & 0xFFu);
    color = expand(pixel & 0x1Fu) |
            (expand((pixel >> 5) & 0x1Fu) << 8u) |
            (expand((pixel >> 10) & 0x1Fu) << 16u) |
            (static_cast<std::uint32_t>(alpha) << 24u);
  } else {
    return vertex_color;
  }
  const auto texture_function = static_cast<unsigned>((tex0 >> 35) & 3u);
  const bool texture_alpha = (tex0 & (1ull << 34)) != 0u;
  const auto traced = [&](std::uint32_t output) {
    if (gs_.pixel_watch_enabled())
      gs_.set_texture_trace({true, raw_u, raw_v, u, v, format,
          texel_address & 0x3FFFFFu, raw_texel, color, vertex_color, output});
    return output;
  };
  if (texture_function == 1u) { // DECAL
    return traced(texture_alpha ? color :
        (color & 0x00FFFFFFu) | (vertex_color & 0xFF000000u));
  }
  const auto vertex_alpha = vertex_color >> 24;
  const auto texel_alpha = color >> 24;
  std::uint32_t output = 0;
  for (unsigned shift = 0; shift < 24u; shift += 8u) {
    const auto texel = (color >> shift) & 0xFFu;
    const auto vertex = (vertex_color >> shift) & 0xFFu;
    const auto highlight = texture_function >= 2u ? vertex_alpha : 0u;
    output |= std::min(255u, ((texel * vertex) >> 7u) + highlight) << shift;
  }
  auto alpha = vertex_alpha; // TCC=RGB preserves incoming alpha in every mode.
  if (texture_alpha) {
    if (texture_function == 0u) alpha = (texel_alpha * vertex_alpha) >> 7u;
    else if (texture_function == 2u) alpha = texel_alpha + vertex_alpha;
    else alpha = texel_alpha; // HIGHLIGHT2
  }
  // Texture-function saturation precedes alpha test and framebuffer blending.
  return traced(output | (std::min(255u, alpha) << 24));
}

void Gif::trace_draw(GsTracePrimitive kind, std::uint64_t sequence, unsigned context) {
  if (!gs_.pixel_watch_enabled()) return;
  gs_.set_draw_trace({kind, sequence, packets_submitted_, prim_, frame_[context],
      tex0_[context], clamp_[context], alpha_[context], test_[context], texa_});
}

void Gif::set_prim(std::uint64_t value) {
  // A PRIM write restarts assembly even if the mode bits are unchanged.
  // Otherwise independent strips/sprites can inherit vertices from a prior draw.
  vertex_count_ = 0;
  have_first_xyz2_ = false;
  prim_ = value;
}

void Gif::write_register(std::uint8_t address, std::uint64_t value) {
  switch (address) {
  case 0x00: set_prim(value); break;
  case 0x01: rgbaq_ = value; break;
  case 0x02: st_ = value; break; // A+D / REGLIST ST does not change Q.
  case 0x03: uv_ = value; break;
  case 0x06: tex0_[0] = value; break;
  case 0x07: tex0_[1] = value; break;
  case 0x14: tex1_[0] = value; break;
  case 0x15: tex1_[1] = value; break;
  case 0x08: clamp_[0] = value; break;
  case 0x09: clamp_[1] = value; break;
  case 0x05: emit_xyz2(value); break;
  case 0x04: emit_xyz2(value & 0x00FFFFFFFFFFFFFFull); break;
  case 0x0C: emit_xyz2(value & 0x00FFFFFFFFFFFFFFull, false); break;
  case 0x0D: emit_xyz2(value, false); break;
  case 0x18: xyoffset_[0] = value; break;
  case 0x19: xyoffset_[1] = value; break;
  case 0x3B: texa_ = value; break;
  case 0x40: scissor_[0] = value; break;
  case 0x41: scissor_[1] = value; break;
  case 0x42: alpha_[0] = value; break;
  case 0x43: alpha_[1] = value; break;
  case 0x46: colclamp_ = (value & 1u) != 0u; break;
  case 0x49: pabe_ = (value & 1u) != 0u; break;
  case 0x47: test_[0] = value; break;
  case 0x48: test_[1] = value; break;
  case 0x4E: zbuf_[0] = value; break;
  case 0x4F: zbuf_[1] = value; break;
  case 0x4C: frame_[0] = value; break;
  case 0x4D: frame_[1] = value; break;
  case 0x50: bitbltbuf_ = value; break;
  case 0x51: trxpos_ = value; break;
  case 0x52: trxreg_ = value; break;
  case 0x53: trxdir_ = value; transfer_pixels_ = 0; break;
  default: break;
  }
}

void Gif::emit_xyz2(std::uint64_t value, bool draw) {
  const auto primitive = static_cast<unsigned>(prim_ & 7u);
  const auto context = static_cast<unsigned>((prim_ >> 9) & 1u);
  gs_.set_color_target(&local_memory_, frame_[context]);
  gs_.set_blend_state((prim_ & (1u << 6)) != 0u, alpha_[context], pabe_, colclamp_);
  gs_.set_alpha_test(test_[context]);
  const auto clip = scissor_[context];
  gs_.set_scissor(static_cast<int>((clip & 0x7FFu) / 4u),
                  static_cast<int>(((clip >> 32) & 0x7FFu) / 4u),
                  static_cast<int>(((clip >> 16) & 0x7FFu) / 4u),
                  static_cast<int>(((clip >> 48) & 0x7FFu) / 4u));
  if ((clip & 0x7FFu) > ((clip >> 16) & 0x7FFu) ||
      ((clip >> 32) & 0x7FFu) > ((clip >> 48) & 0x7FFu))
    gs_.set_scissor(1, 1, 0, 0);
  const bool zte = (test_[context] & (1ull << 16)) != 0;
  const auto ztst = static_cast<Gs::DepthTest>((test_[context] >> 17) & 3u);
  // ZTE=0 bypasses comparison AND suppresses depth writes. ZMSK only
  // suppresses writes; it must not bypass an enabled comparison.
  gs_.set_depth_state(zte ? ztst : Gs::DepthTest::Always,
                      zte && (zbuf_[context] & (1ull << 32)) == 0);
  const auto make_vertex = [&](std::uint64_t xyz) {
    return GsVertex{
        scaled_coordinate(xyz, xyoffset_[context], 0u, 0u),
        scaled_coordinate(xyz, xyoffset_[context], 16u, 32u),
        static_cast<std::uint32_t>(xyz >> 32),
        static_cast<std::uint32_t>(rgbaq_), st_, uv_,
        static_cast<std::uint32_t>(rgbaq_ >> 32)};
  };

  if (primitive == 0u) {
    if (draw) {
      trace_draw(GsTracePrimitive::Point, points_emitted_, context);
      gs_.point(make_vertex(value));
      ++points_emitted_;
    }
    return;
  }
  if (primitive == 1u || primitive == 2u) {
    const auto vertex = make_vertex(value);
    if (vertex_count_ != 0u) {
      if (draw) {
        trace_draw(GsTracePrimitive::Line, lines_emitted_, context);
        auto first = vertices_[0];
        if ((prim_ & (1u << 3)) == 0u) first.color = vertex.color;
        gs_.line(first, vertex);
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
      triangle_xyz_[vertex_count_] = value;
      vertices_[vertex_count_++] = vertex;
      return;
    }
    if (draw) {
      trace_draw(GsTracePrimitive::Triangle, triangles_emitted_, context);
      if ((trace_triangles_ && (triangle_records_.size() < 64u ||
                               nondegenerate_triangle_records_.size() < 64u)) ||
          capture_sprite_enabled_) {
        const GifTriangleRecord record{{vertices_[0], vertices_[1], vertex},
            {triangle_xyz_[0], triangle_xyz_[1], value},
            prim_, xyoffset_[context], scissor_[context], test_[context],
            zbuf_[context], tex0_[context], clamp_[context], frame_[context],
            alpha_[context], triangles_emitted_};
        if (trace_triangles_ && triangle_records_.size() < 64u)
          triangle_records_.push_back(record);
        // Host-space area only: this selects useful raster inputs, not proof
        // of visibility or native GS coverage. Keep the original prefix too.
        const auto area = std::int64_t{vertices_[1].x - vertices_[0].x} *
                            (vertex.y - vertices_[0].y) -
                          std::int64_t{vertices_[1].y - vertices_[0].y} *
                            (vertex.x - vertices_[0].x);
        if (area != 0) {
          if (trace_triangles_ && nondegenerate_triangle_records_.size() < 64u)
            nondegenerate_triangle_records_.push_back(record);
          if (capture_sprite_enabled_) {
            recent_triangles_[recent_triangle_cursor_] = record;
            recent_triangle_cursor_ = (recent_triangle_cursor_ + 1u) %
                                      recent_triangles_.size();
            recent_triangle_count_ = std::min(recent_triangle_count_ + 1u,
                                              recent_triangles_.size());
          }
        }
      }
      auto first = vertices_[0];
      auto second = vertices_[1];
      // Flat shading uses the drawing kick's color. Keep the assembly
      // vertices intact: a subsequent strip/fan may enable interpolation.
      if ((prim_ & (1u << 3)) == 0u)
        first.color = second.color = vertex.color;
      if ((prim_ & 0x10u) != 0u) {
        const bool fixed_uv = (prim_ & 0x100u) != 0u;
        gs_.triangle(first, second, vertex,
            [this, context](unsigned u, unsigned v, std::uint32_t color) {
              return sample_texture(context, u, v, color);
            }, fixed_uv ? 0u : 1u << ((tex0_[context] >> 26) & 15u),
               fixed_uv ? 0u : 1u << ((tex0_[context] >> 30) & 15u));
      } else {
        gs_.triangle(first, second, vertex);
      }
      ++triangles_emitted_;
    }
    if (primitive == 3u) vertex_count_ = 0u;
    else if (primitive == 4u) {
      triangle_xyz_[0] = triangle_xyz_[1];
      triangle_xyz_[1] = value;
      vertices_[0] = vertices_[1];
      vertices_[1] = vertex;
    } else {
      triangle_xyz_[1] = value;
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
  trace_draw(GsTracePrimitive::Sprite, sprites_emitted_, context);
  if (capture_sprite_enabled_ && sprites_emitted_ == capture_sprite_sequence_) {
    const auto first = (recent_triangle_cursor_ + recent_triangles_.size() -
                        recent_triangle_count_) % recent_triangles_.size();
    for (std::size_t index = 0; index < recent_triangle_count_; ++index)
      sprite_preceding_triangles_.push_back(
          recent_triangles_[(first + index) % recent_triangles_.size()]);
    // Preserve the most recent PSMCT16 input before the sprite can feed back
    // into local memory. This intentionally reads Astra's linear model, not
    // a native GS-swizzled surface.
    for (auto it = sprite_preceding_triangles_.rbegin();
         it != sprite_preceding_triangles_.rend(); ++it) {
      const auto tex0 = it->tex0;
      if (((tex0 >> 20u) & 0x3Fu) != 2u) continue;
      const auto width = 1u << ((tex0 >> 26u) & 0xFu);
      const auto height = 1u << ((tex0 >> 30u) & 0xFu);
      const auto stride = static_cast<unsigned>((tex0 >> 14u) & 0x3Fu) * 64u;
      if (stride == 0u || width > 256u || height > 256u) break;
      const auto base = static_cast<std::uint32_t>(tex0 & 0x3FFFu) * 256u;
      sprite_preceding_texture16_tex0_ = tex0;
      sprite_preceding_texture16_width_ = width;
      sprite_preceding_texture16_height_ = height;
      sprite_preceding_texture16_.reserve(width * height);
      for (unsigned y = 0; y < height; ++y)
        for (unsigned x = 0; x < width; ++x) {
          const auto address = base + (y * stride + x) * 2u;
          sprite_preceding_texture16_.push_back(static_cast<std::uint16_t>(
              local_memory_[address & 0x3FFFFFu] |
              (local_memory_[(address + 1u) & 0x3FFFFFu] << 8u)));
        }
      break;
    }
  }
  const bool record_sprite = trace_triangles_ && sprite_records_.size() < 256u;
  if (record_sprite) {
    sprite_records_.push_back({first_xyz2_, value, first_uv_, uv_, prim_,
        xyoffset_[context], scissor_[context], tex0_[context], tex1_[context],
        clamp_[context], frame_[context], test_[context], alpha_[context],
        texa_, sprites_emitted_});
    auto& record = sprite_records_.back();
    record.rgbaq = rgbaq_;
    if ((prim_ & (1u << 4)) != 0u) {
      const auto tex0 = tex0_[context];
      const auto source_width = static_cast<unsigned>((tex0 >> 14u) & 0x3Fu) * 64u;
      const auto format = static_cast<unsigned>((tex0 >> 20u) & 0x3Fu);
      if (source_width != 0u && format <= 1u) {
        const auto source_base = static_cast<std::uint32_t>(tex0 & 0x3FFFu) * 256u;
        record.source_hash = 1469598103934665603ull;
        for (unsigned y = 0; y < kSpriteSourceProbeHeight; ++y)
          for (unsigned x = 0; x < kSpriteSourceProbeWidth; ++x) {
            const auto pixel = read_local32(source_base +
                ((y * 4u) * source_width + x * 4u) * 4u);
            record.source_hash ^= pixel;
            record.source_hash *= 1099511628211ull;
            if ((pixel & 0xFFFFFFu) != 0u) ++record.source_nonzero_rgb;
          }
      }
    }
  }
  if (capture_sprite_enabled_ && sprites_emitted_ == capture_sprite_sequence_) {
    // Freeze the *raw linear* color source before rendering feedback into the
    // destination. This is a diagnostic grid, not swizzled GS texture decode.
    const auto tex0 = tex0_[context];
    const auto source_width = static_cast<unsigned>((tex0 >> 14u) & 0x3Fu) * 64u;
    const auto format = static_cast<unsigned>((tex0 >> 20u) & 0x3Fu);
    if (source_width != 0u && format <= 1u) {
      const auto source_base = static_cast<std::uint32_t>(tex0 & 0x3FFFu) * 256u;
      sprite_texture_capture_.reserve(kSpriteSourceProbeWidth *
                                      kSpriteSourceProbeHeight);
      for (unsigned y = 0; y < kSpriteSourceProbeHeight; ++y)
        for (unsigned x = 0; x < kSpriteSourceProbeWidth; ++x)
          sprite_texture_capture_.push_back(read_local32(source_base +
              ((y * 4u) * source_width + x * 4u) * 4u));
    }
  }
  const auto origin_x = fixed_coordinate(first_xyz2_, xyoffset_[context], 0u, 0u);
  const auto origin_y = fixed_coordinate(first_xyz2_, xyoffset_[context], 16u, 32u);
  const auto end_x = fixed_coordinate(value, xyoffset_[context], 0u, 0u);
  const auto end_y = fixed_coordinate(value, xyoffset_[context], 16u, 32u);
  const auto span_x = end_x - origin_x;
  const auto span_y = end_y - origin_y;
  // Preserve fractional bounds until coverage is decided. This selects the
  // native anchors lying in [min, max), not a native-resolution GS raster.
  auto x0 = ceil_quarter_coordinate(origin_x);
  auto y0 = ceil_quarter_coordinate(origin_y);
  auto x1 = ceil_quarter_coordinate(end_x);
  auto y1 = ceil_quarter_coordinate(end_y);
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);

  const auto scissor = scissor_[context];
  const auto clip_x0 = static_cast<int>(((scissor & 0x7FFu) + 3u) / 4u);
  const auto clip_x1 = static_cast<int>(((scissor >> 16) & 0x7FFu) / 4u);
  const auto clip_y0 = static_cast<int>((((scissor >> 32) & 0x7FFu) + 3u) / 4u);
  const auto clip_y1 = static_cast<int>(((scissor >> 48) & 0x7FFu) / 4u);
  x0 = std::max(x0, clip_x0);
  x1 = std::min(x1, clip_x1 + 1);
  y0 = std::max(y0, clip_y0);
  y1 = std::min(y1, clip_y1 + 1);
  const auto z = static_cast<std::uint32_t>(value >> 32);
  const auto color = static_cast<std::uint32_t>(rgbaq_);
  const bool textured_uv = (prim_ & (1u << 4)) != 0u &&
                           (prim_ & (1u << 8)) != 0u;
  const auto u0 = static_cast<int>(first_uv_ & 0x3FFFu);
  const auto v0 = static_cast<int>((first_uv_ >> 16) & 0x3FFFu);
  const auto u1 = static_cast<int>(uv_ & 0x3FFFu);
  const auto v1 = static_cast<int>((uv_ >> 16) & 0x3FFFu);
  for (auto y = y0; y < y1; ++y) {
    for (auto x = x0; x < x1; ++x) {
      auto pixel_color = color;
      if (textured_uv) {
        // A single rational division retains UV fractions and affine prestep
        // from the original (possibly reversed) geometry. Truncating a signed
        // slope before adding UV0 would choose the wrong texel at boundaries.
        const auto u = (std::int64_t{u0} * span_x +
            std::int64_t{x * 64 - origin_x} * (u1 - u0)) /
            (std::int64_t{span_x} * 16);
        const auto v = (std::int64_t{v0} * span_y +
            std::int64_t{y * 64 - origin_y} * (v1 - v0)) /
            (std::int64_t{span_y} * 16);
        pixel_color = sample_texture(context,
            static_cast<unsigned>(std::max<std::int64_t>(0, u)),
            static_cast<unsigned>(std::max<std::int64_t>(0, v)), color);
      }
      gs_.point({x, y, z, pixel_color});
    }
  }
  if (capture_sprite_enabled_ && sprites_emitted_ == capture_sprite_sequence_) {
    sprite_framebuffer_capture_.reserve(Gs::kWidth * Gs::kHeight);
    for (int y = 0; y < Gs::kHeight; ++y)
      for (int x = 0; x < Gs::kWidth; ++x)
        sprite_framebuffer_capture_.push_back(gs_.pixel(x, y));
    capture_sprite_enabled_ = false;
  }
  if (record_sprite) {
    auto& record = sprite_records_.back();
    record.target_hash = 1469598103934665603ull;
    for (int y = 0; y < Gs::kHeight; ++y)
      for (int x = 0; x < Gs::kWidth; ++x) {
        const auto pixel = gs_.pixel(x, y);
        record.target_hash ^= pixel;
        record.target_hash *= 1099511628211ull;
        if ((pixel & 0xFFFFFFu) != 0u) ++record.target_nonzero_rgb;
      }
  }
  ++sprites_emitted_;
  have_first_xyz2_ = false;
}

} // namespace ps2vita
