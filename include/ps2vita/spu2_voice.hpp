#pragma once
#include "ps2vita/spu2_adpcm.hpp"
#include "ps2vita/spu2_envelope.hpp"

namespace ps2vita {
// Functional mono voice for integration tests. tick() is one output sample.
// Sample holding is NOT hardware Gaussian interpolation. No KON delay, NAX/
// ENDX timing, register binding, IRQ, volume sweep, routing or reverb yet.
class Spu2Voice {
public:
  void configure(std::uint16_t pitch, std::uint16_t adsr1, std::uint16_t adsr2) {
    pitch_ = pitch > 0x3FFFu ? 0x3FFFu : pitch;
    envelope_.configure(adsr1, adsr2);
  }
  void key_on(std::uint32_t address) {
    stream_.start(address); envelope_.key_on();
    index_ = 28; pending_ = 4096; sample_ = 0;
    consumed_ = 0; active_ = true; decode_error_ = false;
  }
  void key_off() { envelope_.key_off(); }
  void set_loop_address(std::uint32_t address) { stream_.set_loop_address(address); }
  std::int16_t tick(const Memory& memory);
  bool active() const { return active_; }
  bool decode_error() const { return decode_error_; }
  std::uint64_t samples_consumed() const { return consumed_; }
  std::uint16_t envelope_level() const { return envelope_.level(); }
private:
  Spu2AdpcmStream stream_;
  Spu2Envelope envelope_;
  Spu2AdpcmBlock block_;
  unsigned index_ = 28, pending_ = 0;
  std::uint16_t pitch_ = 4096;
  std::int16_t sample_ = 0;
  std::uint64_t consumed_ = 0;
  bool active_ = false, decode_error_ = false;
};
}
