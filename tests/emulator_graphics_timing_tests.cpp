#include "ps2vita/emulator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, bool chain, std::uint32_t phase,
           std::uint32_t slice, const char* name) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL [%s phase=%u slice=%u]: %s\n",
                 chain ? "chain" : "normal", phase, slice, name);
  }
}

constexpr std::uint32_t kEntry = 0x1000u;
constexpr std::uint32_t kTag = 0x2FF0u;
constexpr std::uint32_t kPayload = 0x3000u;
constexpr std::uint32_t kOutput = 0x4000u;
constexpr std::uint32_t kVuOffset = 0x1230u;
constexpr std::array<std::uint32_t, 8> kUnpack{{
    0u, 0x01000101u, 0u, 0x6C010123u,
    0x3F800000u, 0xBF000000u, 0x00000001u, 0xDEADBEEFu}};

std::array<std::uint32_t, 20> make_program(bool chain) {
  // The guest, not this host test, enables DMA, programs its source/count,
  // kicks VIF1, polls STR, reads VU1 memory and stores the observed value.
  return {{
      0x3C081001u, // lui t0, 0x1001: signed SW offsets address EE hardware
      0x34090001u, // ori t1, zero, DMAE
      0xAD09E000u, // sw t1, D_CTRL(t0)
      0x34093000u, // ori t1, zero, payload
      0xAD099010u, // sw t1, D1_MADR(t0)
      0x34090002u, // ori t1, zero, QWC=2
      0xAD099020u, // sw t1, D1_QWC(t0)
      0x34092FF0u, // ori t1, zero, tag
      0xAD099030u, // sw t1, D1_TADR(t0)
      chain ? 0x34090105u : 0x34090101u, // source chain or normal, DIR=1, STR=1
      0xAD099000u, // sw t1, D1_CHCR(t0)
      0x8D099000u, // poll: lw t1, D1_CHCR(t0)
      0x31290100u, // andi t1, t1, STR
      0x1520FFFDu, // bne t1, zero, poll
      0u,         // branch delay slot
      0x3C0A1100u, // lui t2, 0x1100
      0x354AC000u, // ori t2, t2, VU1 data bank
      0x8D4B1230u, // lw t3, uploaded VU1 qword's X lane
      0xAC0B4000u, // sw t3, result(zero), before host graphics service can hide it
      0x0000000Du  // break: bounded host-test completion, not a PS2 exit service
  }};
}

std::vector<std::uint8_t> make_elf(bool chain, std::uint32_t phase) {
  const auto program = make_program(chain);
  constexpr std::size_t segment_size = kOutput + 16u - kEntry;
  std::vector<std::uint8_t> elf(0x100u + segment_size);
  const auto put = [&](std::size_t offset, std::uint32_t value, unsigned bytes) {
    for (unsigned byte = 0; byte < bytes; ++byte)
      elf[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  put(0u, 0x464C457Fu, 4u); put(4u, 0x010101u, 3u);
  put(16u, 2u, 2u); put(18u, 8u, 2u); put(20u, 1u, 4u);
  put(24u, kEntry, 4u); put(28u, 52u, 4u);
  put(40u, 52u, 2u); put(42u, 32u, 2u); put(44u, 1u, 2u);
  put(52u, 1u, 4u); put(56u, 0x100u, 4u);
  put(60u, kEntry, 4u); put(64u, kEntry, 4u);
  put(68u, static_cast<std::uint32_t>(segment_size), 4u);
  put(72u, static_cast<std::uint32_t>(segment_size), 4u);
  put(76u, 7u, 4u); put(80u, 16u, 4u);
  for (std::size_t word = 0; word < program.size(); ++word)
    put(0x100u + (phase + word) * 4u, program[word], 4u);
  // END tag: two inline qwords. Normal mode must ignore these tag bytes.
  put(0x100u + kTag - kEntry, 0x70000002u, 4u);
  for (std::size_t word = 0; word < kUnpack.size(); ++word)
    put(0x100u + kPayload - kEntry + word * 4u, kUnpack[word], 4u);
  return elf;
}

struct Result {
  ps2vita::CpuState cpu{};
  std::uint32_t output = 0u;
};

bool same_ee_architecture(const ps2vita::CpuState& lhs,
                          const ps2vita::CpuState& rhs) {
  // Compare fields explicitly, not structure padding or host AOT telemetry.
  return lhs.gpr == rhs.gpr && lhs.gpr_hi == rhs.gpr_hi &&
      lhs.cop0 == rhs.cop0 && lhs.fpr == rhs.fpr && lhs.fcr == rhs.fcr &&
      lhs.fpu_acc == rhs.fpu_acc && lhs.vu0_vf == rhs.vu0_vf &&
      lhs.vu0_vf_hi == rhs.vu0_vf_hi && lhs.vu0_vi == rhs.vu0_vi &&
      lhs.vu0_acc == rhs.vu0_acc && lhs.hi == rhs.hi && lhs.lo == rhs.lo &&
      lhs.hi1 == rhs.hi1 && lhs.lo1 == rhs.lo1 &&
      lhs.shift_amount == rhs.shift_amount && lhs.pc == rhs.pc &&
      lhs.cycles == rhs.cycles;
}

Result run_fixture(bool chain, std::uint32_t phase, std::uint32_t slice) {
  ps2vita::Emulator emulator;
  const auto elf = make_elf(chain, phase);
  const auto loaded = emulator.load_elf(elf.data(), elf.size());
  check(loaded.ok && loaded.entry == kEntry && loaded.segments == 1u,
        chain, phase, slice, "owned EE ELF loads");
  auto& memory = emulator.memory();
  memory.write32(kOutput, 0xA55AA55Au);
  for (unsigned lane = 0; lane < 4u; ++lane)
    memory.vu1_store_data_word(kVuOffset + lane * 4u, 0x13579BDFu);
  check(emulator.run_slice(0u) == ps2vita::StopReason::StepLimit &&
            emulator.cpu().state().cycles == 0u &&
            memory.read32(kOutput) == 0xA55AA55Au,
        chain, phase, slice, "zero budget does not execute guest instructions");
  auto reason = ps2vita::StopReason::StepLimit;
  constexpr std::uint32_t limit = 4096u;
  while (reason == ps2vita::StopReason::StepLimit &&
         emulator.cpu().state().cycles < limit) {
    const auto before = emulator.cpu().state().cycles;
    const auto remaining = limit - static_cast<std::uint32_t>(before);
    reason = emulator.run_slice(std::min(slice, remaining));
    if (emulator.cpu().state().cycles == before) break;
  }
  const auto& cpu = emulator.cpu().state();
  check(reason == ps2vita::StopReason::Break && cpu.cycles < limit,
        chain, phase, slice, "STR poll reaches its bounded guest completion");
  // The existing transport model discovers the kick on its SW cycle and
  // completes its two qwords 16 modeled cycles later. The polling loop then
  // finishes at cycle 36 plus the entry NOP phase. This is a model oracle,
  // not measured PS2 timing.
  check(cpu.cycles == 36u + phase && cpu.pc == kEntry + (19u + phase) * 4u &&
            cpu.cop0[9] == 36u + phase && cpu.fast_path_instructions == 0u,
        chain, phase, slice, "guest cycle/PC/count result is exact without a fast fill");
  check(cpu.gpr[8] == 0x10010000u && cpu.gpr[9] == 0u &&
            cpu.gpr[10] == ps2vita::Memory::kVu1DataBase &&
            cpu.gpr[11] == kUnpack[4] && cpu.gpr[0] == 0u,
        chain, phase, slice, "guest registers contain the uploaded word after STR clears");
  check(memory.read32(kOutput) == kUnpack[4], chain, phase, slice,
        "guest saved the uploaded value before stopping, not the stale poison");
  bool all_lanes = true;
  for (unsigned lane = 0; lane < 4u; ++lane)
    all_lanes = all_lanes &&
        memory.vu1_data_word(kVuOffset + lane * 4u) == kUnpack[4u + lane];
  check(all_lanes, chain, phase, slice, "UNPACK preserves all four raw lanes");
  check(emulator.vif1().packets_submitted() == 1u &&
            emulator.vif1().packets_rejected() == 0u &&
            emulator.vif1().vectors_unpacked() == 1u &&
            emulator.vif1().vu1().pairs_executed() == 0u,
        chain, phase, slice, "one completed DMA is serviced exactly once without running VU");
  check(memory.read32(0x10009010u) == kPayload + 32u &&
            memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u &&
            (memory.read32(0x1000E010u) & 2u) != 0u &&
            memory.read32(0x10009030u) == (chain ? kPayload + 32u : kTag),
        chain, phase, slice, "DMA register completion agrees with the consumed source");
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) && !memory.pop_gif_packet(packet) &&
            emulator.gif().pending_bytes() == 0u,
        chain, phase, slice, "no completed transport or GIF payload remains queued");
  return {cpu, memory.read32(kOutput)};
}

struct BiosResult {
  Result ee{};
  ps2vita::IopState iop{};
};

BiosResult run_bios_fixture(bool chain, std::uint32_t phase,
                            std::uint32_t slice) {
  ps2vita::Emulator emulator;
  // An owned, minimal reset ROM tests the BIOS scheduling route. It does not
  // contain Sony firmware and does not claim accuracy of the original intro.
  std::vector<std::uint8_t> rom(ps2vita::Memory::kBiosSize);
  auto program = make_program(chain);
  program.back() = 0x1000FFFFu; // beq zero, zero, self; next ROM word is NOP
  const auto put = [&](std::size_t offset, std::uint32_t value) {
    for (unsigned byte = 0; byte < 4u; ++byte)
      rom[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8u));
  };
  for (std::size_t word = 0; word < program.size(); ++word)
    put((phase + word) * 4u, program[word]);
  put(0x1000u, 0x1000FFFFu); // Separate harmless IOP self-branch/NOP loop.
  check(emulator.load_bios(rom.data(), rom.size()) && emulator.boot_bios(),
        chain, phase, slice, "owned reset ROM boots the BIOS scheduler route");
  // Reset clears hidden branch/load state before selecting the isolated IOP
  // entry. It never runs the EE DMA program as IOP code.
  emulator.iop().reset();
  emulator.iop().state().pc = 0xBFC01000u;
  auto& memory = emulator.memory();
  // ROM loading does not initialize RAM. These are fixture input bytes, not
  // host submissions: only the EE's stores above can initiate their transfer.
  memory.write32(kTag, 0x70000002u);
  for (std::size_t word = 0; word < kUnpack.size(); ++word)
    memory.write32(kPayload + static_cast<std::uint32_t>(word * 4u), kUnpack[word]);
  memory.write32(kOutput, 0xA55AA55Au);
  for (unsigned lane = 0; lane < 4u; ++lane)
    memory.vu1_store_data_word(kVuOffset + lane * 4u, 0x13579BDFu);
  auto reason = ps2vita::StopReason::StepLimit;
  constexpr std::uint32_t limit = 64u;
  while (reason == ps2vita::StopReason::StepLimit &&
         emulator.cpu().state().cycles < limit) {
    const auto before = emulator.cpu().state().cycles;
    const auto remaining = limit - static_cast<std::uint32_t>(before);
    reason = emulator.run_slice(std::min(slice, remaining));
    if (emulator.cpu().state().cycles == before) break;
  }
  const auto& cpu = emulator.cpu().state();
  const auto& iop = emulator.iop().state();
  check(reason == ps2vita::StopReason::StepLimit && cpu.cycles == limit &&
            cpu.pc == 0xBFC00000u + (20u + phase) * 4u &&
            cpu.cop0[9] == limit && cpu.fast_path_instructions == 0u,
        chain, phase, slice, "BIOS guest reaches its terminal branch within 64 EE cycles");
  check(cpu.gpr[8] == 0x10010000u && cpu.gpr[9] == 0u &&
            cpu.gpr[10] == ps2vita::Memory::kVu1DataBase &&
            cpu.gpr[11] == kUnpack[4] && cpu.gpr[0] == 0u,
        chain, phase, slice, "BIOS guest registers observe uploaded VU data after STR clears");
  check(memory.read32(kOutput) == kUnpack[4], chain, phase, slice,
        "BIOS guest saved uploaded data instead of stale poison");
  bool all_lanes = true;
  for (unsigned lane = 0; lane < 4u; ++lane)
    all_lanes = all_lanes &&
        memory.vu1_data_word(kVuOffset + lane * 4u) == kUnpack[4u + lane];
  check(all_lanes, chain, phase, slice, "BIOS route preserves all UNPACK raw lanes");
  check(emulator.vif1().packets_submitted() == 1u &&
            emulator.vif1().packets_rejected() == 0u &&
            emulator.vif1().vectors_unpacked() == 1u &&
            emulator.vif1().vu1().pairs_executed() == 0u,
        chain, phase, slice, "BIOS route services exactly one VIF packet without running VU");
  check(memory.read32(0x10009010u) == kPayload + 32u &&
            memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u &&
            (memory.read32(0x1000E010u) & 2u) != 0u &&
            memory.read32(0x10009030u) == (chain ? kPayload + 32u : kTag),
        chain, phase, slice, "BIOS route publishes consumed DMA source registers");
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) && !memory.pop_gif_packet(packet) &&
            emulator.gif().pending_bytes() == 0u,
        chain, phase, slice, "BIOS route leaves no completed packet queued");
  check(iop.cycles == 8u && iop.pc == 0xBFC01000u &&
            emulator.iop().stop_reason() == ps2vita::IopStopReason::None &&
            iop.gpr[0] == 0u,
        chain, phase, slice, "BIOS route preserves eight-cycle EE/IOP scheduling ratio");
  return {{cpu, memory.read32(kOutput)}, iop};
}

} // namespace

int main() {
  // Whole-run equality catches host batching changing guest-visible state.
  // No artificial FLUSHE, manual VIF submission or host service call is used.
  constexpr std::array<std::uint32_t, 6> slices{{1u, 2u, 7u, 8u, 64u, 4096u}};
  for (const bool chain : {false, true}) {
    // A six-NOP phase moves completion to cycle 33 and the guest LW to 40.
    // Servicing only every eight cycles would still read before servicing
    // the packet at cycle 40. The device event, not merely a small fixed
    // batching quantum, must provide the visibility boundary.
    for (const std::uint32_t phase : {0u, 6u}) {
      const auto baseline = run_fixture(chain, phase, slices[0]);
      for (std::size_t index = 1u; index < slices.size(); ++index) {
        const auto result = run_fixture(chain, phase, slices[index]);
        check(same_ee_architecture(result.cpu, baseline.cpu) &&
                  result.output == baseline.output,
              chain, phase, slices[index],
              "architectural result is invariant under host slice size");
      }
      const auto bios_baseline = run_bios_fixture(chain, phase, slices[0]);
      for (std::size_t index = 1u; index < slices.size(); ++index) {
        const auto result = run_bios_fixture(chain, phase, slices[index]);
        check(same_ee_architecture(result.ee.cpu, bios_baseline.ee.cpu) &&
                  result.ee.output == bios_baseline.ee.output &&
                  result.iop.gpr == bios_baseline.iop.gpr &&
                  result.iop.cop0 == bios_baseline.iop.cop0 &&
                  result.iop.hi == bios_baseline.iop.hi &&
                  result.iop.lo == bios_baseline.iop.lo &&
                  result.iop.pc == bios_baseline.iop.pc &&
                  result.iop.cycles == bios_baseline.iop.cycles,
              chain, phase, slices[index],
              "BIOS EE/IOP architectural result is invariant under host slice size");
      }
    }
  }
  std::printf("emulator_graphics_timing_tests: %u checks, %u failures\n",
              checks, failures);
  return failures == 0u ? 0 : 1;
}
