#pragma once

#include "ps2vita/cpu.hpp"

#include <cstddef>
#include <cstdint>

namespace ps2vita {

// Every translated function returns through this small ABI. It avoids exposing
// recompiler-specific runtime classes on Vita and gives the dispatcher enough
// information to continue natively, return to the interpreter, or stop.
enum class AotExitKind : std::uint8_t {
  Direct,
  Indirect,
  Interpreter,
  Stop,
};

struct AotExit {
  AotExitKind kind = AotExitKind::Interpreter;
  StopReason reason = StopReason::None;
  std::uint32_t target = 0;
  std::uint32_t instructions = 0;
};

using AotFunction = AotExit (*)(Memory&, CpuState&);

struct AotFunctionEntry {
  std::uint32_t guest_start = 0;
  std::uint32_t guest_end = 0;
  AotFunction function = nullptr;
};

struct AotPackage {
  const char* name = nullptr;
  const AotFunctionEntry* entries = nullptr;
  std::size_t entry_count = 0;
  std::uint32_t abi_version = 0;
  std::uint32_t guest_address_min = 0;
  std::uint32_t guest_address_max = 0;
  // SHA-256 of the generator's length-delimited ELF input set.
  const char* source_fingerprint_sha256 = nullptr;
};

constexpr std::uint32_t kAotAbiVersion = 1u;

enum class AotPackageError : std::uint8_t {
  None,
  MissingName,
  UnsupportedAbi,
  MissingFingerprint,
  InvalidFingerprint,
  MissingEntries,
  InvalidAddressRange,
  MisalignedEntry,
  InvalidEntryRange,
  EntryOutsidePackage,
  UnsortedEntries,
  OverlappingEntries,
  MissingFunction,
};

struct AotPackageValidation {
  AotPackageError error = AotPackageError::None;
  std::size_t entry_index = 0;

  constexpr bool ok() const { return error == AotPackageError::None; }
};

AotPackageValidation validate_aot_package(const AotPackage& package);

const AotFunctionEntry* find_aot_function(const AotPackage& package,
                                          std::uint32_t pc);
AotExit execute_aot(const AotPackage& package, Memory& memory, CpuState& state);
AotExit dispatch_aot(const AotPackage& package, Memory& memory, CpuState& state,
                     std::uint32_t function_budget);
const AotPackage& generated_phase0_aot_package();
const AotPackage& phase0_aot_package();

// Phase-0 uses a fixed table of exact callable/resumable guest entry points.
// Later title packages can provide the same structure per resident
// module/overlay without growing AstraRT itself.
const AotFunctionEntry* find_phase0_aot_function(std::uint32_t pc);
AotExit execute_phase0_aot(Memory& memory, CpuState& state);
AotExit dispatch_phase0_aot(Memory& memory, CpuState& state,
                            std::uint32_t function_budget);

// Phase-0 contract between an offline EE recompiler and AstraRT. The initial
// probe deliberately contains no renderer, BIOS, or operating-system surface:
// it proves that native code can preserve guest-visible CPU and RAM state.
struct AotProbeResult {
  bool matched = false;
  StopReason interpreter_stop = StopReason::None;
  StopReason aot_stop = StopReason::None;
  std::uint32_t interpreter_value = 0;
  std::uint32_t aot_value = 0;
  std::uint64_t interpreter_v0 = 0;
  std::uint64_t aot_v0 = 0;
};

AotProbeResult run_phase0_aot_probe();
AotProbeResult run_phase0_aot_chain_probe();

using AotClockMicroseconds = std::uint64_t (*)();

struct AotBenchmarkResult {
  bool matched = false;
  StopReason interpreter_stop = StopReason::None;
  StopReason aot_stop = StopReason::None;
  std::uint32_t iterations = 0;
  std::uint32_t samples = 0;
  std::uint64_t guest_instructions = 0;
  std::uint64_t interpreter_microseconds = 0;
  std::uint64_t aot_microseconds = 0;
  std::uint64_t interpreter_checksum = 0;
  std::uint64_t aot_checksum = 0;
  std::uint32_t speedup_x100 = 0;
};

// Executes the same generated synthetic guest workload through the portable
// EE interpreter and AstraRT. Setup, allocation, and screen rendering are
// outside the measured regions; reported time is the median sample.
AotBenchmarkResult run_phase0_aot_benchmark(
    AotClockMicroseconds clock_microseconds,
    std::uint32_t iterations = 4096u, std::uint32_t samples = 5u);

} // namespace ps2vita
