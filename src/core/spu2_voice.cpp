#include "ps2vita/spu2_voice.hpp"

namespace ps2vita {
bool spu2_fixed_volume(std::int16_t sample, std::uint16_t volume, std::int32_t& output) {
  if ((volume & 0x8000u) != 0u) return false;
  const int gain = (int(volume & 0x7FFFu) - ((volume & 0x4000u) ? 32768 : 0)) * 2;
  const int product = int(sample) * gain;
  output = product >= 0 ? product / 32768 : -((-product + 32767) / 32768);
  return true;
}
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
