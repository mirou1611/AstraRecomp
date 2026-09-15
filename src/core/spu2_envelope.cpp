#include "ps2vita/spu2_envelope.hpp"
#include <algorithm>

namespace ps2vita {
std::uint16_t Spu2Envelope::tick() {
  if (phase_ == Phase::Stopped) return level_;
  unsigned shift = 0;
  int step = -8;
  bool decreasing = true, exponential = false;
  switch (phase_) {
  case Phase::Attack:
    shift = (adsr1_ >> 10) & 31u;
    step = 7 - ((adsr1_ >> 8) & 3u);
    decreasing = false; exponential = (adsr1_ & 0x8000u) != 0;
    break;
  case Phase::Decay:
    shift = (adsr1_ >> 4) & 15u; exponential = true;
    break;
  case Phase::Sustain:
    shift = (adsr2_ >> 8) & 31u;
    decreasing = (adsr2_ & 0x4000u) != 0;
    step = decreasing ? -8 + int((adsr2_ >> 6) & 3u) :
                        7 - int((adsr2_ >> 6) & 3u);
    exponential = (adsr2_ & 0x8000u) != 0;
    break;
  case Phase::Release:
    shift = adsr2_ & 31u; exponential = (adsr2_ & 0x20u) != 0;
    break;
  case Phase::Stopped: break;
  }
  unsigned increment = 32768u >> (shift > 11u ? shift - 11u : 0u);
  int delta = step * int(1u << (shift < 11u ? 11u - shift : 0u));
  if (exponential && decreasing) {
    const int product = delta * int(level_);
    delta = product >= 0 ? product / 32768 : -((-product + 32767) / 32768);
  } else if (exponential && level_ > 0x6000u) {
    increment /= 4u;
  }
  counter_ += std::max(1u, increment);
  if (counter_ >= 32768u) {
    counter_ = 0;
    level_ = static_cast<std::uint16_t>(std::clamp(int(level_) + delta, 0, 32767));
  }
  if (phase_ == Phase::Attack && level_ == 32767) phase_ = Phase::Decay;
  else if (phase_ == Phase::Decay && level_ <= ((adsr1_ & 15u) + 1u) * 2048u)
    phase_ = Phase::Sustain;
  else if ((phase_ == Phase::Sustain || phase_ == Phase::Release) && level_ == 0)
    phase_ = Phase::Stopped;
  return level_;
}
}
