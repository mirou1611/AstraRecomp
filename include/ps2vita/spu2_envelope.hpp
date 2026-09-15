#pragma once
#include <cstdint>

namespace ps2vita {
// Functional envelope clocked once per audio sample. Not yet wired to KON/KOFF
// or ENVX. Extreme-rate behavior follows a minimum counter increment of one;
// hardware "infinite" settings remain unverified.
class Spu2Envelope {
public:
  enum class Phase { Stopped, Attack, Decay, Sustain, Release };
  void configure(std::uint16_t adsr1, std::uint16_t adsr2) {
    adsr1_ = adsr1; adsr2_ = adsr2;
  }
  void key_on() { phase_ = Phase::Attack; level_ = 0; counter_ = 0; }
  void key_off() {
    if (phase_ != Phase::Stopped) { phase_ = Phase::Release; counter_ = 0; }
  }
  std::uint16_t tick();
  std::uint16_t level() const { return level_; }
  Phase phase() const { return phase_; }
private:
  std::uint16_t adsr1_ = 0, adsr2_ = 0, level_ = 0;
  std::uint32_t counter_ = 0;
  Phase phase_ = Phase::Stopped;
};
}
