#include "ps2vita/aot.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace {

std::uint64_t clock_microseconds() {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace

int main(int argc, char** argv) {
  const auto iterations = argc >= 2
      ? static_cast<std::uint32_t>(std::strtoul(argv[1], nullptr, 0))
      : 100000u;
  const auto samples = argc >= 3
      ? static_cast<std::uint32_t>(std::strtoul(argv[2], nullptr, 0)) : 9u;
  const auto result = ps2vita::run_phase0_aot_benchmark(
      clock_microseconds, iterations, samples);
  std::printf(
      "matched=%u iterations=%u samples=%u guest_instructions=%llu "
      "interpreter_us=%llu aot_us=%llu speedup_x100=%u\n",
      result.matched ? 1u : 0u, result.iterations, result.samples,
      static_cast<unsigned long long>(result.guest_instructions),
      static_cast<unsigned long long>(result.interpreter_microseconds),
      static_cast<unsigned long long>(result.aot_microseconds),
      result.speedup_x100);
  return result.matched ? 0 : 1;
}
