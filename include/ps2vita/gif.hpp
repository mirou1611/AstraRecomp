#pragma once

#include "ps2vita/gs.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace ps2vita {

struct GifImageRecord {
  std::uint64_t tag = 0;
  std::uint64_t bitbltbuf = 0;
  std::uint64_t trxpos = 0;
  std::uint64_t trxreg = 0;
  std::uint64_t trxdir = 0;
  std::uint64_t first_qword = 0;
  std::uint64_t hash = 0;
  std::uint64_t bytes = 0;
};

struct GifTriangleRecord {
  std::array<GsVertex, 3> vertices{};
  std::array<std::uint64_t, 3> xyz{};
  std::uint64_t prim = 0, xyoffset = 0, scissor = 0, test = 0, zbuf = 0;
  std::uint64_t tex0 = 0, clamp = 0, frame = 0, alpha = 0;
  std::uint64_t sequence = 0;
};

struct GifSpriteRecord {
  std::uint64_t first_xyz = 0, second_xyz = 0;
  std::uint64_t first_uv = 0, second_uv = 0;
  std::uint64_t prim = 0, xyoffset = 0, scissor = 0, tex0 = 0, tex1 = 0;
  std::uint64_t clamp = 0, frame = 0, test = 0, alpha = 0, texa = 0;
  std::uint64_t sequence = 0;
  std::uint64_t rgbaq = 0, source_hash = 0, target_hash = 0;
  std::uint32_t source_nonzero_rgb = 0, target_nonzero_rgb = 0;
};

// GIF packet frontend. It owns guest GS register state while Gs remains the
// small host raster backend.
class Gif {
public:
  static constexpr unsigned kSpriteSourceProbeWidth = 160;
  static constexpr unsigned kSpriteSourceProbeHeight = 64;
  explicit Gif(Gs& gs);
  ~Gif() { gs_.set_color_target(nullptr, 0u); }
  void reset();
  void enable_triangle_trace(bool enabled) { trace_triangles_ = enabled; }
  const std::vector<GifTriangleRecord>& triangle_records() const {
    return triangle_records_;
  }
  const std::vector<GifTriangleRecord>& nondegenerate_triangle_records() const {
    return nondegenerate_triangle_records_;
  }
  const std::vector<GifSpriteRecord>& sprite_records() const {
    return sprite_records_;
  }
  void capture_sprite_framebuffer_at(std::uint64_t sequence) {
    capture_sprite_sequence_ = sequence;
    capture_sprite_enabled_ = true;
    sprite_framebuffer_capture_.clear();
    sprite_texture_capture_.clear();
  }
  bool sprite_framebuffer_captured() const {
    return !sprite_framebuffer_capture_.empty();
  }
  const std::vector<std::uint32_t>& sprite_framebuffer_capture() const {
    return sprite_framebuffer_capture_;
  }
  const std::vector<std::uint32_t>& sprite_texture_capture() const {
    return sprite_texture_capture_;
  }
  bool submit(const std::uint8_t* data, std::size_t size);
  std::uint64_t packets_submitted() const { return packets_submitted_; }
  std::uint64_t packets_rejected() const { return packets_rejected_; }
  std::uint64_t sprites_emitted() const { return sprites_emitted_; }
  std::uint64_t points_emitted() const { return points_emitted_; }
  std::uint64_t lines_emitted() const { return lines_emitted_; }
  std::uint64_t triangles_emitted() const { return triangles_emitted_; }
  std::uint64_t packed_tags() const { return packed_tags_; }
  std::uint64_t reglist_tags() const { return reglist_tags_; }
  std::uint64_t image_tags() const { return image_tags_; }
  std::uint64_t image_bytes() const { return image_bytes_; }
  std::size_t pending_bytes() const { return pending_.size(); }
  std::uint64_t first_image_bitbltbuf() const { return first_image_bitbltbuf_; }
  std::uint64_t first_image_trxpos() const { return first_image_trxpos_; }
  std::uint64_t first_image_trxreg() const { return first_image_trxreg_; }
  std::uint64_t first_image_trxdir() const { return first_image_trxdir_; }
  const std::vector<GifImageRecord>& image_records() const {
    return image_records_;
  }
  std::uint32_t read_local32(std::uint32_t byte_address) const;
  std::uint64_t local_bytes_written() const { return local_bytes_written_; }
  std::uint64_t first_unsupported_tag() const { return first_unsupported_tag_; }

private:
  void set_prim(std::uint64_t value);
  void write_register(std::uint8_t address, std::uint64_t value);
  void write_image(const std::uint8_t* data, std::size_t size);
  std::uint32_t sample_texture(unsigned context, unsigned u, unsigned v,
                               std::uint32_t vertex_color) const;
  void emit_xyz2(std::uint64_t value, bool draw = true);

  Gs& gs_;
  bool trace_triangles_ = false;
  std::vector<GifTriangleRecord> triangle_records_;
  std::vector<GifTriangleRecord> nondegenerate_triangle_records_;
  std::vector<GifSpriteRecord> sprite_records_;
  std::vector<std::uint32_t> sprite_framebuffer_capture_;
  std::vector<std::uint32_t> sprite_texture_capture_;
  std::uint64_t capture_sprite_sequence_ = 0;
  bool capture_sprite_enabled_ = false;
  std::vector<std::uint8_t> local_memory_;
  std::uint64_t prim_ = 0;
  std::uint64_t rgbaq_ = 0x8000000080808080ull;
  std::uint64_t st_ = 0;
  std::uint32_t packed_q_ = 0x3F800000u;
  std::uint64_t tex0_[2]{};
  std::uint64_t tex1_[2]{};
  std::uint64_t clamp_[2]{};
  // PSMCT32/24 bind the logical linear color target; other formats are pending.
  std::uint64_t frame_[2]{};
  std::uint64_t texa_ = 0;
  std::uint64_t test_[2]{};
  std::uint64_t zbuf_[2]{};
  std::uint64_t alpha_[2]{};
  bool pabe_ = false, colclamp_ = false;
  std::uint64_t uv_ = 0;
  std::uint64_t xyoffset_[2]{};
  std::uint64_t scissor_[2]{0x07FF000007FF0000ull,
                            0x07FF000007FF0000ull};
  std::uint64_t bitbltbuf_ = 0;
  std::uint64_t trxpos_ = 0;
  std::uint64_t trxreg_ = 0;
  std::uint64_t trxdir_ = 0;
  std::size_t transfer_pixels_ = 0;
  std::uint64_t first_xyz2_ = 0;
  std::uint64_t first_uv_ = 0;
  bool have_first_xyz2_ = false;
  std::array<GsVertex, 3> vertices_{};
  std::array<std::uint64_t, 3> triangle_xyz_{};
  unsigned vertex_count_ = 0;
  std::vector<std::uint8_t> pending_;
  std::uint64_t packets_submitted_ = 0;
  std::uint64_t packets_rejected_ = 0;
  std::uint64_t sprites_emitted_ = 0;
  std::uint64_t points_emitted_ = 0;
  std::uint64_t lines_emitted_ = 0;
  std::uint64_t triangles_emitted_ = 0;
  std::uint64_t packed_tags_ = 0;
  std::uint64_t reglist_tags_ = 0;
  std::uint64_t image_tags_ = 0;
  std::uint64_t image_bytes_ = 0;
  std::uint64_t first_image_bitbltbuf_ = 0;
  std::uint64_t first_image_trxpos_ = 0;
  std::uint64_t first_image_trxreg_ = 0;
  std::uint64_t first_image_trxdir_ = 0;
  std::vector<GifImageRecord> image_records_;
  std::uint64_t local_bytes_written_ = 0;
  std::uint64_t first_unsupported_tag_ = 0;
};

} // namespace ps2vita
