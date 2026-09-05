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

// GIF packet frontend. It owns guest GS register state while Gs remains the
// small host raster backend.
class Gif {
public:
  explicit Gif(Gs& gs);
  void reset();
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
  void emit_xyz2(std::uint64_t value);

  Gs& gs_;
  std::vector<std::uint8_t> local_memory_;
  std::uint64_t prim_ = 0;
  std::uint64_t rgbaq_ = 0x8000000080808080ull;
  std::uint64_t tex0_[2]{};
  std::uint64_t uv_ = 0;
  std::uint64_t xyoffset_[2]{};
  std::uint64_t scissor_[2]{0x07FF000007FF0000ull,
                            0x07FF000007FF0000ull};
  std::uint64_t bitbltbuf_ = 0;
  std::uint64_t trxpos_ = 0;
  std::uint64_t trxreg_ = 0;
  std::uint64_t trxdir_ = 0;
  std::uint64_t first_xyz2_ = 0;
  std::uint64_t first_uv_ = 0;
  bool have_first_xyz2_ = false;
  std::array<GsVertex, 3> vertices_{};
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
