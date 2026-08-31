#include "ps2vita/aot.hpp"

#include "ps2vita/memory.hpp"
#include "aot_benchmark_program.hpp"

#include <algorithm>
#include <array>

namespace ps2vita {
namespace {

constexpr std::uint32_t kEntry = 0x1000u;
constexpr std::uint32_t kResultAddress = 0x2000u;
constexpr std::uint32_t kChainEntry = 0x3000u;
constexpr std::uint32_t kChainLeaf = 0x3020u;
constexpr std::uint32_t kChainResultAddress = 0x2004u;

void install_probe_program(Memory& memory) {
  memory.write32(kEntry + 0u, 0x2402002Au); // addiu v0, zero, 42
  memory.write32(kEntry + 4u, 0xAC022000u); // sw    v0, 0x2000(zero)
  memory.write32(kEntry + 8u, 0x0000000Du); // break
}

void install_chain_program(Memory& memory) {
  memory.write32(kChainEntry + 0u, 0x0C000C08u);  // jal   0x3020
  memory.write32(kChainEntry + 4u, 0x24040028u);  // addiu a0, zero, 40
  memory.write32(kChainEntry + 8u, 0x24420002u);  // addiu v0, v0, 2
  memory.write32(kChainEntry + 12u, 0xAC022004u); // sw    v0, 0x2004
  memory.write32(kChainEntry + 16u, 0x0000000Du); // break
  memory.write32(kChainLeaf + 0u, 0x00801021u);   // addu  v0, a0, zero
  memory.write32(kChainLeaf + 4u, 0x03E00008u);   // jr    ra
  memory.write32(kChainLeaf + 8u, 0x00000000u);   // nop (delay slot)
}

void prepare_benchmark(Memory& memory, CpuState& state,
                       std::uint32_t iterations) {
  memory.clear();
  for (std::size_t i = 0; i < benchmark_program::kWords.size(); ++i)
    memory.write32(benchmark_program::kEntry + static_cast<std::uint32_t>(i * 4u),
                   benchmark_program::kWords[i]);
  for (std::uint32_t word = 0; word < 16u; ++word)
    memory.write32(benchmark_program::kSource + word * 4u,
                   0x10203040u + word * 0x01030507u);
  for (std::uint32_t word = 0; word < 16u; ++word)
    memory.write32(benchmark_program::kDestination + word * 4u, 0u);
  state = {};
  state.pc = benchmark_program::kEntry;
  state.gpr[4] = iterations;
  state.gpr[5] = 0x1122334455667788ull;
  state.gpr_hi[5] = 0x8877665544332211ull;
  state.gpr[6] = 0x00FF00FF00FF00FFull;
  state.gpr_hi[6] = 0x0F0F0F0F0F0F0F0Full;
  state.gpr[7] = 0xF0F0F0F0F0F0F0F0ull;
  state.gpr_hi[7] = 0xFF00FF00FF00FF00ull;
}

std::uint64_t benchmark_checksum(const Memory& memory, const CpuState& state) {
  std::uint64_t hash = 1469598103934665603ull;
  auto mix = [&](std::uint64_t value) {
    hash ^= value;
    hash *= 1099511628211ull;
  };
  mix(state.gpr[2]);
  mix(state.gpr_hi[2]);
  mix(state.gpr[3]);
  mix(state.hi);
  mix(state.lo);
  for (std::uint32_t offset = 0u; offset < 64u; offset += 8u)
    mix(memory.read64(benchmark_program::kDestination + offset));
  return hash;
}

std::uint64_t median_sample(std::array<std::uint64_t, 9>& values,
                            std::uint32_t count) {
  std::sort(values.begin(), values.begin() + count);
  return values[count / 2u];
}

bool is_lower_hex(char value) {
  return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

AotExit execute_validated_aot(const AotPackage& package, Memory& memory,
                              CpuState& state) {
  const AotFunctionEntry* entry = find_aot_function(package, state.pc);
  if (!entry || !entry->function)
    return {AotExitKind::Interpreter, StopReason::None, state.pc, 0u};
  return entry->function(memory, state);
}

} // namespace

AotPackageValidation validate_aot_package(const AotPackage& package) {
  if (!package.name || package.name[0] == '\0')
    return {AotPackageError::MissingName, 0u};
  if (package.abi_version != kAotAbiVersion)
    return {AotPackageError::UnsupportedAbi, 0u};
  if (!package.source_fingerprint_sha256)
    return {AotPackageError::MissingFingerprint, 0u};
  for (std::size_t i = 0; i < 64u; ++i) {
    if (!is_lower_hex(package.source_fingerprint_sha256[i]))
      return {AotPackageError::InvalidFingerprint, 0u};
  }
  if (package.source_fingerprint_sha256[64] != '\0')
    return {AotPackageError::InvalidFingerprint, 0u};
  if (!package.entries || package.entry_count == 0u)
    return {AotPackageError::MissingEntries, 0u};
  if (package.guest_address_min >= package.guest_address_max ||
      (package.guest_address_min & 3u) != 0u ||
      (package.guest_address_max & 3u) != 0u)
    return {AotPackageError::InvalidAddressRange, 0u};

  for (std::size_t i = 0; i < package.entry_count; ++i) {
    const AotFunctionEntry& entry = package.entries[i];
    if ((entry.guest_start & 3u) != 0u || (entry.guest_end & 3u) != 0u)
      return {AotPackageError::MisalignedEntry, i};
    if (entry.guest_start >= entry.guest_end)
      return {AotPackageError::InvalidEntryRange, i};
    if (entry.guest_start < package.guest_address_min ||
        entry.guest_end > package.guest_address_max)
      return {AotPackageError::EntryOutsidePackage, i};
    if (!entry.function)
      return {AotPackageError::MissingFunction, i};
    if (i != 0u) {
      const AotFunctionEntry& previous = package.entries[i - 1u];
      if (entry.guest_start <= previous.guest_start)
        return {AotPackageError::UnsortedEntries, i};
      if (entry.guest_start < previous.guest_end)
        return {AotPackageError::OverlappingEntries, i};
    }
  }
  return {};
}

const AotFunctionEntry* find_aot_function(const AotPackage& package,
                                          std::uint32_t pc) {
  // Generated package tables are sorted. Binary search remains allocation-free
  // and scales to thousands of functions/resume points without a Vita-side map.
  if (!package.entries || package.entry_count == 0u)
    return nullptr;
  std::size_t first = 0;
  std::size_t last = package.entry_count;
  while (first < last) {
    const std::size_t middle = first + (last - first) / 2u;
    const AotFunctionEntry& entry = package.entries[middle];
    if (pc < entry.guest_start) {
      last = middle;
    } else if (pc > entry.guest_start) {
      first = middle + 1u;
    } else {
      return &entry;
    }
  }
  return nullptr;
}

AotExit execute_aot(const AotPackage& package, Memory& memory, CpuState& state) {
  if (!validate_aot_package(package).ok())
    return {AotExitKind::Interpreter, StopReason::None, state.pc, 0u};
  return execute_validated_aot(package, memory, state);
}

AotExit dispatch_aot(const AotPackage& package, Memory& memory, CpuState& state,
                     std::uint32_t function_budget) {
  if (!validate_aot_package(package).ok())
    return {AotExitKind::Interpreter, StopReason::None, state.pc, 0u};

  std::uint32_t instructions = 0;
  for (std::uint32_t i = 0; i < function_budget; ++i) {
    AotExit exit = execute_validated_aot(package, memory, state);
    instructions += exit.instructions;
    if (exit.kind != AotExitKind::Direct &&
        exit.kind != AotExitKind::Indirect) {
      exit.instructions = instructions;
      return exit;
    }
  }

  // A bounded escape is essential on Vita: a bad title package or an endless
  // native call cycle must always be able to yield to the portable path.
  return {AotExitKind::Interpreter, StopReason::None, state.pc, instructions};
}

const AotPackage& phase0_aot_package() {
  return generated_phase0_aot_package();
}

const AotFunctionEntry* find_phase0_aot_function(std::uint32_t pc) {
  return find_aot_function(phase0_aot_package(), pc);
}

AotExit execute_phase0_aot(Memory& memory, CpuState& state) {
  return execute_aot(phase0_aot_package(), memory, state);
}

AotExit dispatch_phase0_aot(Memory& memory, CpuState& state,
                            std::uint32_t function_budget) {
  return dispatch_aot(phase0_aot_package(), memory, state, function_budget);
}

AotProbeResult run_phase0_aot_probe() {
  Memory interpreter_memory;
  install_probe_program(interpreter_memory);
  Cpu interpreter(interpreter_memory);
  interpreter.reset(kEntry);

  AotProbeResult result;
  result.interpreter_stop = interpreter.run(4u);
  result.interpreter_value = interpreter_memory.read32(kResultAddress);
  result.interpreter_v0 = interpreter.state().gpr[2];

  Memory aot_memory;
  install_probe_program(aot_memory);
  CpuState aot_state{};
  aot_state.pc = kEntry;
  const AotExit exit = execute_phase0_aot(aot_memory, aot_state);
  result.aot_stop = exit.reason;
  result.aot_value = aot_memory.read32(kResultAddress);
  result.aot_v0 = aot_state.gpr[2];

  result.matched =
      result.interpreter_stop == result.aot_stop &&
      result.interpreter_value == result.aot_value &&
      result.interpreter_v0 == result.aot_v0 &&
      interpreter.state().pc == aot_state.pc &&
      interpreter.state().cycles == aot_state.cycles &&
      exit.kind == AotExitKind::Stop && exit.target == kEntry + 8u &&
      exit.instructions == 3u;
  return result;
}

AotProbeResult run_phase0_aot_chain_probe() {
  Memory interpreter_memory;
  install_chain_program(interpreter_memory);
  Cpu interpreter(interpreter_memory);
  interpreter.reset(kChainEntry);

  AotProbeResult result;
  result.interpreter_stop = interpreter.run(16u);
  result.interpreter_value = interpreter_memory.read32(kChainResultAddress);
  result.interpreter_v0 = interpreter.state().gpr[2];

  Memory aot_memory;
  install_chain_program(aot_memory);
  CpuState aot_state{};
  aot_state.pc = kChainEntry;
  const AotExit exit = dispatch_phase0_aot(aot_memory, aot_state, 4u);

  result.aot_stop = exit.reason;
  result.aot_value = aot_memory.read32(kChainResultAddress);
  result.aot_v0 = aot_state.gpr[2];
  result.matched =
      result.interpreter_stop == result.aot_stop &&
      result.interpreter_value == result.aot_value &&
      result.interpreter_v0 == result.aot_v0 &&
      interpreter.state().pc == aot_state.pc &&
      interpreter.state().cycles == aot_state.cycles &&
      exit.kind == AotExitKind::Stop && exit.instructions == 8u;
  return result;
}

AotBenchmarkResult run_phase0_aot_benchmark(
    AotClockMicroseconds clock_microseconds, std::uint32_t iterations,
    std::uint32_t samples) {
  AotBenchmarkResult result;
  result.iterations = std::max(1u, std::min(1000000u, iterations));
  result.samples = std::max(1u, std::min(9u, samples));
  const auto instruction_budget = result.iterations * 128u + 256u;
  const auto function_budget = result.iterations * 16u + 256u;
  std::array<std::uint64_t, 9> interpreter_times{};
  std::array<std::uint64_t, 9> aot_times{};
  Memory memory;
  CpuState interpreter_state{};
  CpuState aot_state{};
  AotExit aot_exit{};

  // Untimed warm-up pays first-touch and cold instruction-cache costs for both
  // paths before the median samples used by the go/no-go result.
  prepare_benchmark(memory, interpreter_state, 8u);
  {
    Cpu interpreter(memory);
    interpreter.reset(benchmark_program::kEntry);
    interpreter.state() = interpreter_state;
    interpreter.run(8u * 128u + 256u);
  }
  prepare_benchmark(memory, aot_state, 8u);
  dispatch_phase0_aot(memory, aot_state, 8u * 16u + 256u);

  for (std::uint32_t sample = 0; sample < result.samples; ++sample) {
    prepare_benchmark(memory, interpreter_state, result.iterations);
    Cpu interpreter(memory);
    interpreter.reset(benchmark_program::kEntry);
    interpreter.state() = interpreter_state;
    const auto interpreter_begin = clock_microseconds ? clock_microseconds() : 0u;
    result.interpreter_stop = interpreter.run(instruction_budget);
    const auto interpreter_end = clock_microseconds ? clock_microseconds() : 0u;
    interpreter_times[sample] = interpreter_end - interpreter_begin;
    interpreter_state = interpreter.state();
    result.interpreter_checksum = benchmark_checksum(memory, interpreter_state);

    prepare_benchmark(memory, aot_state, result.iterations);
    const auto aot_begin = clock_microseconds ? clock_microseconds() : 0u;
    aot_exit = dispatch_phase0_aot(memory, aot_state, function_budget);
    const auto aot_end = clock_microseconds ? clock_microseconds() : 0u;
    aot_times[sample] = aot_end - aot_begin;
    result.aot_stop = aot_exit.reason;
    result.aot_checksum = benchmark_checksum(memory, aot_state);
  }

  result.guest_instructions = interpreter_state.cycles;
  result.interpreter_microseconds = median_sample(interpreter_times, result.samples);
  result.aot_microseconds = median_sample(aot_times, result.samples);
  if (result.aot_microseconds != 0u) {
    result.speedup_x100 = static_cast<std::uint32_t>(std::min<std::uint64_t>(
        999999u, result.interpreter_microseconds * 100u /
                     result.aot_microseconds));
  }
  result.matched = result.interpreter_stop == StopReason::Break &&
      result.aot_stop == StopReason::Break &&
      aot_exit.kind == AotExitKind::Stop &&
      result.interpreter_checksum == result.aot_checksum &&
      interpreter_state.pc == aot_state.pc &&
      interpreter_state.cycles == aot_state.cycles &&
      interpreter_state.gpr == aot_state.gpr &&
      interpreter_state.gpr_hi == aot_state.gpr_hi &&
      interpreter_state.hi == aot_state.hi &&
      interpreter_state.lo == aot_state.lo;
  return result;
}

} // namespace ps2vita
