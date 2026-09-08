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

} // namespace ps2vita
