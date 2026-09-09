#include "ps2vita/spu2_voice.hpp"

namespace ps2vita {
std::int16_t Spu2Voice::tick(const Memory& memory) {
  if (!active_) return 0;
  const auto gain = envelope_.tick();
  if (envelope_.phase() == Spu2Envelope::Phase::Stopped) {
    active_ = false;
    return 0;
  }
  while (pending_ >= 4096u) {
    if (index_ == block_.samples.size()) {
      const bool expected_block = stream_.active();
      if (!stream_.decode_next(memory, block_)) {
        decode_error_ = expected_block;
        active_ = false;
        return 0;
      }
      index_ = 0;
    }
    sample_ = block_.samples[index_++];
    ++consumed_;
    pending_ -= 4096u;
  }
  pending_ += pitch_;
  const int product = int(sample_) * int(gain);
  const int scaled = product >= 0 ? product / 32768 : -((-product + 32767) / 32768);
  return static_cast<std::int16_t>(scaled);
}
}
