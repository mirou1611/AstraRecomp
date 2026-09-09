#include "ps2vita/spu2_adpcm.hpp"

#include <algorithm>

namespace ps2vita {
namespace {
// Arithmetic right shift expressed without implementation-defined negative
// shifts or undefined signed left shifts. Inputs here are safely bounded.
std::int32_t floor_shift(std::int32_t value, unsigned shift) {
  const auto divisor = std::int32_t(1u << shift);
  return value >= 0 ? value / divisor : -((-value + divisor - 1) / divisor);
}
}

bool decode_spu2_adpcm(const std::array<std::uint8_t, 16>& encoded,
                      Spu2AdpcmHistory& history, Spu2AdpcmBlock& output) {
  constexpr std::int32_t coefficients[5][2] = {
      {0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
  const unsigned predictor = encoded[0] >> 4;
  if (predictor >= 5u) return false;
  const unsigned shift = encoded[0] & 15u;
  auto next_history = history;
  Spu2AdpcmBlock decoded;
  decoded.flags = encoded[1];
  for (unsigned i = 0; i < decoded.samples.size(); ++i) {
    const unsigned packed = encoded[2u + i / 2u];
    const unsigned nibble = (packed >> ((i & 1u) * 4u)) & 15u;
    const auto signed_nibble = static_cast<std::int32_t>(nibble) - (nibble >= 8u ? 16 : 0);
    const auto prediction = coefficients[predictor][0] * next_history.previous +
        coefficients[predictor][1] * next_history.previous2;
    const auto sample = std::clamp(floor_shift(signed_nibble * 4096, shift) +
        floor_shift(prediction + 32, 6u), -32768, 32767);
    decoded.samples[i] = static_cast<std::int16_t>(sample);
    next_history.previous2 = next_history.previous;
    next_history.previous = decoded.samples[i];
  }
  history = next_history;
  output = decoded;
  return true;
}
bool Spu2AdpcmStream::decode_next(const std::array<std::uint8_t, 16>& encoded,
                                Spu2AdpcmBlock& output) {
  if (!active_ || !decode_spu2_adpcm(encoded, history_, output)) return false;
  if ((output.flags & 4u) != 0u && !manual_loop_) loop_ = next_;
  if ((output.flags & 1u) != 0u) {
    ended_ = true;
    next_ = loop_;
    active_ = (output.flags & 2u) != 0u;
  } else {
    next_ = (next_ + 8u) & 0xFFFF8u;
  }
  return true;
}
} // namespace ps2vita
