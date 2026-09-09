#pragma once

#include <array>
#include <cstdint>

namespace ps2vita {

struct Spu2AdpcmHistory {
  std::int16_t previous = 0;
  std::int16_t previous2 = 0;
};

struct Spu2AdpcmBlock {
  std::array<std::int16_t, 28> samples{};
  std::uint8_t flags = 0;
};

// Decode one 16-byte SPU2 ADPCM block. History belongs to the voice, not
// the sample address. Unsupported predictor IDs leave output/history intact.
// Loop flags are returned; voice addressing/envelopes/mixing are separate.
bool decode_spu2_adpcm(const std::array<std::uint8_t, 16>& encoded,
                      Spu2AdpcmHistory& history, Spu2AdpcmBlock& output);

// Functional block stream, not the hardware's sample-fetch clock or NAX/ENDX.
// Caller supplies RAM bytes at next_word_address() (SPU2 addresses are words).
// No pitch, ADSR, key-on delay, IRQ timing or device output is modeled here.
class Spu2AdpcmStream {
public:
  void start(std::uint32_t word_address) {
    next_ = loop_ = word_address & 0xFFFF8u;
    history_ = {};
    active_ = true; ended_ = false; manual_loop_ = false;
  }
  void set_loop_address(std::uint32_t word_address) {
    loop_ = word_address & 0xFFFF8u;
    manual_loop_ = true;
  }
  bool decode_next(const std::array<std::uint8_t, 16>& encoded,
                   Spu2AdpcmBlock& output);
  std::uint32_t next_word_address() const { return next_; }
  std::uint32_t loop_word_address() const { return loop_; }
  bool active() const { return active_; }
  bool encountered_end() const { return ended_; }
private:
  Spu2AdpcmHistory history_{};
  std::uint32_t next_ = 0, loop_ = 0;
  bool active_ = false, ended_ = false, manual_loop_ = false;
};

} // namespace ps2vita
