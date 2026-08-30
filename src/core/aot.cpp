#include "ps2vita/aot.hpp"

#include "ps2vita/memory.hpp"

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

} // namespace ps2vita
