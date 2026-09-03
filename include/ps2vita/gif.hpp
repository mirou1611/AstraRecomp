#pragma once

#include "ps2vita/gs.hpp"

#include <cstddef>
#include <cstdint>

namespace ps2vita {

// GIF packet frontend. It owns guest GS register state while Gs remains the
// small host raster backend.
class Gif {
public:
  explicit Gif(Gs& gs) : gs_(gs) {}
  void reset();
  bool submit(const std::uint8_t* data, std::size_t size);
  std::uint64_t packets_submitted() const { return packets_submitted_; }
  std::uint64_t packets_rejected() const { return packets_rejected_; }
  std::uint64_t sprites_emitted() const { return sprites_emitted_; }
  std::uint64_t packed_tags() const { return packed_tags_; }
  std::uint64_t reglist_tags() const { return reglist_tags_; }
  std::uint64_t image_tags() const { return image_tags_; }
  std::uint64_t first_unsupported_tag() const { return first_unsupported_tag_; }

private:
  void write_register(std::uint8_t address, std::uint64_t value);
  void emit_xyz2(std::uint64_t value);

  Gs& gs_;
  std::uint64_t prim_ = 0;
  std::uint64_t rgbaq_ = 0x8000000080808080ull;
  std::uint64_t xyoffset_[2]{};
  std::uint64_t scissor_[2]{0x07FF000007FF0000ull,
                            0x07FF000007FF0000ull};
  std::uint64_t first_xyz2_ = 0;
  bool have_first_xyz2_ = false;
  std::uint64_t packets_submitted_ = 0;
  std::uint64_t packets_rejected_ = 0;
  std::uint64_t sprites_emitted_ = 0;
  std::uint64_t packed_tags_ = 0;
  std::uint64_t reglist_tags_ = 0;
  std::uint64_t image_tags_ = 0;
  std::uint64_t first_unsupported_tag_ = 0;
};

} // namespace ps2vita
