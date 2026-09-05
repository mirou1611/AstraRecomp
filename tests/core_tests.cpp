#include "ps2vita/aot.hpp"
#include "ps2vita/emulator.hpp"
#include "ps2vita/ee_block.hpp"
#include "ps2vita/execution_census.hpp"
#include "ps2vita/gif.hpp"
#include "ps2vita/vif.hpp"
#include "ps2vita/vu.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace {
int failures = 0;

void check(bool condition, const char* label) {
  if (!condition) { std::fprintf(stderr, "FAIL: %s\n", label); ++failures; }
}

constexpr std::uint32_t i_type(unsigned op, unsigned rs, unsigned rt, std::uint16_t imm) {
  return (op << 26) | (rs << 21) | (rt << 16) | imm;
}

void test_execution_census_blocks_and_edges() {
  ps2vita::ExecutionCensus census;
  census.record(0x1000u, i_type(0x09u, 0u, 2u, 1u), 0x8000u, 0x4000u);
  census.record(0x1004u, i_type(0x04u, 2u, 0u, 1u));
  census.record(0x1008u, 0u); // Branch delay slot.
  census.record(0x100Cu, i_type(0x09u, 2u, 2u, 1u), 0x7FF0u, 0x4100u);
  census.record(0x1010u, 0x08000800u); // J 0x2000.
  census.record(0x1014u, 0u);
  census.record(0x2000u, 0x03E00008u, 0x7FE0u, 0x4200u, 0x100Cu); // JR ra.
  census.record(0x2004u, 0u);
  census.record(0x100Cu, i_type(0x09u, 2u, 2u, 1u), 0x7FD0u, 0x4300u);

  const auto blocks = census.blocks();
  const auto edges = census.edges();
  check(census.instruction_count() == 9u && blocks.size() == 3u &&
        blocks[0].pc == 0x1000u && blocks[0].entries == 1u &&
        blocks[1].pc == 0x100Cu && blocks[1].entries == 2u &&
        blocks[1].sp_min == 0x7FD0u && blocks[1].sp_max == 0x7FF0u &&
        blocks[1].gp_min == 0x4100u && blocks[1].gp_max == 0x4300u &&
        blocks[2].pc == 0x2000u && blocks[2].entries == 1u,
        "execution census counts block entries and register ranges");
  check(edges.size() == 3u &&
        edges[0].source == 0x1000u && edges[0].target == 0x100Cu &&
        edges[1].source == 0x100Cu && edges[1].target == 0x2000u &&
        edges[2].source == 0x2000u && edges[2].target == 0x100Cu,
        "execution census records sorted dynamic block edges");
  const auto indirect = census.indirect_targets();
  check(indirect.size() == 1u && indirect[0].site == 0x2000u &&
        indirect[0].target == 0x100Cu && indirect[0].transitions == 1u,
        "execution census records validated indirect branch targets");
  census.record_mmio_read(0x1004u, 0x1000F000u, 4u);
  census.record_mmio_read(0x1004u, 0x1000F000u, 4u);
  census.record_mmio_read(0x1008u, 0x12001000u, 8u);
  const auto mmio_reads = census.mmio_reads();
  check(mmio_reads.size() == 2u && mmio_reads[0].site == 0x1004u &&
        mmio_reads[0].address == 0x1000F000u &&
        mmio_reads[0].width == 4u && mmio_reads[0].reads == 2u &&
        mmio_reads[1].site == 0x1008u,
        "execution census aggregates and sorts MMIO read sites");

  ps2vita::EventCensus events;
  events.record(2u, 100u);
  events.record(2u, 140u);
  events.record(2u, 200u);
  events.record(1u, 90u);
  const auto event_stats = events.events();
  check(event_stats.size() == 2u && event_stats[0].kind == 1u &&
        event_stats[0].count == 1u && event_stats[0].min_gap == 0u &&
        event_stats[1].kind == 2u && event_stats[1].count == 3u &&
        event_stats[1].min_gap == 40u && event_stats[1].max_gap == 60u &&
        event_stats[1].total_gap == 100u,
        "event census records deterministic spacing statistics");

  census.clear();
  census.record(0x3000u, i_type(0x14u, 2u, 0u, 1u)); // Annulled BEQL.
  census.record(0x3008u, 0u);
  check(census.blocks().size() == 2u && census.edges().size() == 1u,
        "execution census handles an annulled branch-likely delay slot");
}

void test_memory_aliases() {
  ps2vita::Memory memory;
  check(memory.read32(0x1000F520) == 0x00001201,
        "DMAC enabler has its EE reset value");
  memory.write32(0x00001000, 0x12345678);
  check(memory.read32(0x80001000) == 0x12345678, "KSEG0 RAM alias");
  check(memory.read32(0xA0001000) == 0x12345678, "KSEG1 RAM alias");
  check(memory.valid(0x02000000), "uninstalled high memory is a null bus region");
  memory.write32(0x0BC1F000, 0xFFFFFFFF);
  check(memory.read32(0x0BC1F000) == 0, "null bus ignores writes and reads zero");
  memory.write32(0x70000000, 0xCAFEBABE);
  check(memory.read32(0x70000000) == 0xCAFEBABE, "EE scratchpad mapping");
  memory.write32(0x10000000, 0x12345678);
  const auto timer0_first = memory.read32(0x10000000);
  const auto timer0_second = memory.read32(0x10000000);
  check(timer0_second == timer0_first, "EE Timer 0 COUNT is prescaled");
  std::uint32_t timer0_later = timer0_second;
  for (unsigned i = 0; i < 32; ++i) timer0_later = memory.read32(0x10000000);
  check(timer0_later > timer0_first, "EE Timer 0 COUNT advances after prescaling");
  memory.write64(0xB2000010, 0x0123456789ABCDEFull);
  check(memory.read64(0x12000010) == 0x0123456789ABCDEFull,
        "GS privileged registers are routed through the uncached alias");
  memory.write32(0x1000F010, 0x00000005);
  check(memory.read32(0x1000F010) == 0x00000005, "INTC mask toggle on");
  memory.write32(0x1000F010, 0x00000001);
  check(memory.read32(0x1000F010) == 0x00000004, "INTC mask toggle off");
  check(memory.read32(0x1000F240) == 0xF0000102u,
        "SBUS control exposes its fixed reset bits");
  memory.write32(0x1000F220, 0x00000004u);
  memory.write32(0x1000F220, 0x00000008u);
  check(memory.read32(0x1000F220) == 0x0000000Cu,
        "EE writes set SBUS main-to-sub flags");
  memory.write32(0x1000F590, 0x0000210C);
  check(memory.read32(0x1000F520) == 0x0000210C,
        "DMAC enable write port updates its read mirror");
  memory.write32(0x1000F430, 0x80210000);
  check(memory.read32(0x1000F430) == 0,
        "RDRAM controller clears command busy state");
  memory.write32(0x1000F410, 0x80000059);
  check(memory.read32(0x1000F410) == 0,
        "memory-controller command completes synchronously");
  check(memory.read32(0x1000F440) == 0x1F &&
        memory.read32(0x1000F440) == 0x1F &&
        memory.read32(0x1000F440) == 0,
        "RDRAM INIT enumerates two retail devices");
  memory.write32(0x1000F430, 0x00230000);
  check(memory.read32(0x1000F440) == 0x0D0D, "RDRAM CNFGA response");
  memory.write32(0x1000F430, 0x00240000);
  check(memory.read32(0x1000F440) == 0x0090, "RDRAM CNFGB response");
  memory.write64(0xBC1FF010, 0x0123456789ABCDEFull);
  check(memory.read64(0x1C1FF010) == 0x0123456789ABCDEFull,
        "EE uncached alias reaches IOP RAM");
  check(memory.read64(0x1C3FF010) == 0x0123456789ABCDEFull,
        "IOP RAM mirrors across the 8 MiB EE window");
  memory.write16(0xBA000008, 3);
  check(memory.read16(0x1A000008) == 3,
        "DVE registers are mapped through the uncached alias");
  memory.write32(0xBF801010, 0xA5A55A5A);
  check(memory.read32(0x1F801010) == 0xA5A55A5A,
        "secret EE mapping reaches IOP hardware registers");
}

void test_bios_mapping_and_boot() {
  std::vector<std::uint8_t> bios(ps2vita::Memory::kBiosSize, 0);
  const std::uint32_t instruction = i_type(0x09, 0, 2, 42);
  bios[0] = static_cast<std::uint8_t>(instruction);
  bios[1] = static_cast<std::uint8_t>(instruction >> 8);
  bios[2] = static_cast<std::uint8_t>(instruction >> 16);
  bios[3] = static_cast<std::uint8_t>(instruction >> 24);
  ps2vita::Emulator emulator;
  check(emulator.load_bios(bios.data(), bios.size()), "4 MiB BIOS accepted");
  check(emulator.memory().read32(0xBFC00000) == instruction, "BIOS KSEG1 alias");
  emulator.memory().write32(0xBFC00000, 0xFFFFFFFF);
  check(emulator.memory().read32(0x1FC00000) == instruction, "BIOS is read-only");
  check(emulator.boot_bios(), "BIOS boot mode starts");
  check(emulator.cpu().state().pc == 0xBFC00000, "BIOS reset vector");
  check(emulator.cpu().state().cop0[12] == 0x70400004u,
        "BIOS boot seeds R5900 Status reset value");
  check(emulator.cpu().state().cop0[16] == 0x440u &&
        emulator.cpu().state().cop0[15] == 0x2E20u,
        "BIOS boot seeds R5900 Config and PRId");
  check(emulator.cpu().state().fcr[0] == 0x2E30u &&
        emulator.cpu().state().fcr[31] == 0x01000001u,
        "BIOS boot seeds EE FPU reset state");
  check(emulator.cpu().step() == ps2vita::StopReason::None, "first BIOS instruction executes");
  check(emulator.cpu().state().gpr[2] == 42, "BIOS-mapped instruction result");

  ps2vita::Emulator scheduled;
  check(scheduled.load_bios(bios.data(), bios.size()) && scheduled.boot_bios(),
        "scheduler test BIOS boots");
  check(scheduled.run_slice(7) == ps2vita::StopReason::StepLimit &&
        scheduled.iop().state().cycles == 0,
        "BIOS scheduler waits eight EE cycles before stepping IOP");
  check(scheduled.run_slice(1) == ps2vita::StopReason::StepLimit &&
        scheduled.iop().state().cycles == 1,
        "BIOS scheduler preserves the EE-to-IOP phase across slices");
}

void test_iop_memory_and_cpu() {
  ps2vita::Memory memory;
  memory.iop_write32(0x00001000u, 0x12345678u);
  check(memory.iop_read32(0x00201000u) == 0x12345678u,
        "IOP RAM mirrors across its 8 MiB physical window");
  memory.iop_write32(0x1F800100u, 0xA5A55A5Au);
  check(memory.iop_read32(0xBF800100u) == 0xA5A55A5Au,
        "IOP scratchpad maps through KSEG1");
  memory.iop_write32(0xFFFE0130u, 0x00000804u);
  check(memory.iop_read32(0xFFFE0130u) == 0x00000804u,
        "IOP cache-control register retains reset-ROM writes");
  memory.write32(0x1000F220u, 0x0000000Cu);
  memory.iop_write32(0x1D000020u, 0x00000004u);
  check(memory.read32(0x1000F220u) == 0x00000008u,
        "IOP writes clear shared MSFLAG bits");
  memory.iop_write32(0x1D000030u, 0x00000020u);
  check((memory.read32(0x1000F230u) & 0x20u) != 0,
        "IOP writes set shared SMFLAG bits");
  memory.write32(0x1000F220u, 0x00010000u);
  memory.iop_write32(0x1D000020u, 0x00010000u);
  memory.iop_write32(0x1D000030u, 0x00050000u);
  check((memory.read32(0x1000F220u) & 0x00010000u) == 0 &&
        (memory.read32(0x1000F230u) & 0x00050000u) == 0x00050000u,
        "guest IOP SIFINIT and BOOTEND writes reach the EE");
  memory.write32(0x1000F230u, 0x00010000u);
  check((memory.read32(0x1000F230u) & 0x00010000u) == 0,
        "EE writes clear guest-generated SMFLAG bits");

  std::vector<std::uint8_t> bios(ps2vita::Memory::kBiosSize, 0);
  const auto reset_instruction = i_type(0x09, 0, 8, 42);
  bios[0] = static_cast<std::uint8_t>(reset_instruction);
  bios[1] = static_cast<std::uint8_t>(reset_instruction >> 8);
  bios[2] = static_cast<std::uint8_t>(reset_instruction >> 16);
  bios[3] = static_cast<std::uint8_t>(reset_instruction >> 24);
  check(memory.load_bios(bios.data(), bios.size()), "IOP test BIOS accepted");
  ps2vita::IopCpu iop(memory);
  check(iop.state().pc == 0xBFC00000u &&
        iop.state().cop0[12] == 0x00400000u,
        "IOP reset seeds R3000A reset state");
  check(iop.step() == ps2vita::IopStopReason::None &&
        iop.state().gpr[8] == 42,
        "IOP executes from the shared BIOS reset vector");

  iop.state() = {};
  memory.iop_write32(0x00000000u, i_type(0x09, 0, 8, 5));
  memory.iop_write32(0x00000004u, i_type(0x04, 8, 8, 2));
  memory.iop_write32(0x00000008u, i_type(0x09, 0, 9, 7));
  memory.iop_write32(0x0000000Cu, i_type(0x09, 0, 9, 99));
  memory.iop_write32(0x00000010u, i_type(0x2B, 0, 9, 0x1000));
  iop.run(4);
  check(iop.state().pc == 0x00000014u &&
        memory.iop_read32(0x1000u) == 7u,
        "IOP branch delay slot and RAM store execute correctly");

  iop.state() = {};
  iop.state().cop0[12] = 0x00010000u;
  iop.state().gpr[9] = 0xDEADBEEFu;
  memory.iop_write32(0x00000000u, i_type(0x2B, 0, 9, 0x1000));
  memory.iop_write32(0x00001000u, 0x12345678u);
  iop.step();
  check(memory.iop_read32(0x1000u) == 0x12345678u,
        "IOP cache isolation suppresses ordinary RAM stores");

  iop.state() = {};
  iop.state().gpr[8] = 9u;
  memory.iop_write32(0x00000100u, 42u);
  memory.iop_write32(0x00000000u, i_type(0x23, 0, 8, 0x100));
  memory.iop_write32(0x00000004u, i_type(0x09, 8, 9, 1));
  memory.iop_write32(0x00000008u, i_type(0x09, 8, 10, 1));
  iop.run(3);
  check(iop.state().gpr[9] == 10u && iop.state().gpr[10] == 43u,
        "IOP models the R3000A one-instruction load delay");
}

void test_sif1_dma_and_external_interrupts() {
  ps2vita::Memory memory;
  // Observed BIOS source chain: REF followed by REFE. Each block begins with
  // an IOP destination tag and is followed by its word payload.
  memory.write32(0x2000u, 0x30000002u);
  memory.write32(0x2004u, 0x00003000u);
  memory.write32(0x2010u, 0x00000002u);
  memory.write32(0x2014u, 0x00003040u);
  memory.write32(0x3000u, 0x80001000u);
  memory.write32(0x3004u, 4u);
  memory.write32(0x3010u, 0x11111111u);
  memory.write32(0x3014u, 0x22222222u);
  memory.write32(0x3018u, 0x33333333u);
  memory.write32(0x301Cu, 0x44444444u);
  memory.write32(0x3040u, 0xC0001100u);
  memory.write32(0x3044u, 4u);
  memory.write32(0x3050u, 0x55555555u);
  memory.write32(0x3054u, 0x66666666u);
  memory.write32(0x3058u, 0x77777777u);
  memory.write32(0x305Cu, 0x88888888u);
  memory.write32(0x1000C430u, 0x2000u);
  memory.write32(0x1000C400u, 0x184u);
  memory.iop_write32(0x1F801538u, 0x41000300u);
  memory.iop_write32(0x1F801074u, 1u << 3);
  memory.iop_write32(0x1F801078u, 1u);

  memory.advance(1u);
  memory.advance(31u);
  check(memory.iop_read32(0x1000u) == 0u,
        "scheduled SIF1 chain is not visible before its full-QWC deadline");
  memory.advance(1u);
  check(memory.iop_read32(0x1000u) == 0x11111111u &&
        memory.iop_read32(0x100Cu) == 0x44444444u &&
        memory.iop_read32(0x1100u) == 0x55555555u &&
        memory.iop_read32(0x110Cu) == 0x88888888u,
        "SIF1 REF/REFE chain copies EE packets into IOP RAM");
  check((memory.read32(0x1000C400u) & 0x100u) == 0u &&
        (memory.iop_read32(0x1F801538u) & 0x01000000u) == 0u,
        "SIF1 completion clears both channel start bits");
  check((memory.read32(0x1000E010u) & (1u << 6)) != 0u &&
        (memory.iop_read32(0x1F801574u) & (1u << 27)) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 3)) != 0u,
        "SIF1 completion raises EE and IOP DMA status");
  check(memory.iop_interrupt_pending(),
        "IOP INTC exposes the SIF1 DMA completion");

  memory.write32(0x1000E010u, 1u << 22);
  check(memory.ee_interrupt_lines() == 0x800u,
        "EE DMAC mask exposes the pending channel on Cause IP3");
  memory.write32(0x80000200u, 0x24000000u);
  ps2vita::Cpu ee(memory);
  ee.reset(0x1000u);
  ee.set_exception_mode(true);
  ee.state().cop0[12] = 0x00010801u;
  check(ee.step() == ps2vita::StopReason::None &&
        ee.state().pc == 0x80000200u && ee.state().cop0[14] == 0x1000u,
        "EE takes an enabled external DMAC interrupt at the interrupt vector");

  ps2vita::IopCpu iop(memory);
  iop.state().pc = 0x100u;
  iop.state().cop0[12] = 0x00000401u;
  check(iop.step() == ps2vita::IopStopReason::None &&
        iop.state().pc == 0x80000080u && iop.state().cop0[14] == 0x100u,
        "IOP takes an enabled INTC interrupt");

  memory.write32(0x1000E010u, 1u << 6);
  memory.iop_write32(0x1F801070u, ~std::uint32_t{1u << 3});
  check(memory.ee_interrupt_lines() == 0u && !memory.iop_interrupt_pending(),
        "guest acknowledgements lower both DMA interrupt lines");
  memory.iop_write32(0x1F801450u, 2u);
  memory.write32(0x1000F010u, 1u << 1);
  check(memory.ee_interrupt_lines() == 0x400u,
        "IOP ICFG bit 1 raises the EE SBUS interrupt");
}

void test_sif1_continuation_chain() {
  ps2vita::Memory memory;
  memory.write32(0x2000u, 0x30000002u);
  memory.write32(0x2004u, 0x00003000u);
  memory.write32(0x2010u, 0x30000001u);
  memory.write32(0x2014u, 0x00003040u);
  memory.write32(0x2020u, 0x00000002u);
  memory.write32(0x2024u, 0x00003080u);

  memory.write32(0x3000u, 0x00001000u);
  memory.write32(0x3004u, 8u);
  for (unsigned word = 0; word < 4u; ++word)
    memory.write32(0x3010u + word * 4u, 0x11110000u + word);
  for (unsigned word = 0; word < 4u; ++word)
    memory.write32(0x3040u + word * 4u, 0x22220000u + word);
  memory.write32(0x3080u, 0xC0001100u);
  memory.write32(0x3084u, 4u);
  for (unsigned word = 0; word < 4u; ++word)
    memory.write32(0x3090u + word * 4u, 0x33330000u + word);

  memory.write32(0x1000C430u, 0x2000u);
  memory.write32(0x1000C400u, 0x184u);
  memory.iop_write32(0x1F801538u, 0x41000300u);
  memory.advance(1u);
  memory.advance(39u);
  check(memory.iop_read32(0x1000u) == 0u,
        "SIF1 continuation chain waits for its complete QWC deadline");
  memory.advance(1u);
  check(memory.iop_read32(0x1000u) == 0x11110000u &&
        memory.iop_read32(0x100Cu) == 0x11110003u &&
        memory.iop_read32(0x1010u) == 0x22220000u &&
        memory.iop_read32(0x101Cu) == 0x22220003u &&
        memory.iop_read32(0x1100u) == 0x33330000u &&
        memory.iop_read32(0x110Cu) == 0x33330003u,
        "SIF1 REF continuation carries one transfer across source tags");
  check((memory.read32(0x1000C400u) & 0x100u) == 0u &&
        memory.read32(0x1000C430u) == 0x2030u,
        "SIF1 continuation completes and advances through REFE");
}

void test_sif1_zero_length_next_chain() {
  ps2vita::Memory memory;
  // First packet: REF, followed by the zero-QWC NEXT shape observed in the
  // retail BIOS. NEXT jumps to a separately allocated REFE packet.
  memory.write32(0x2000u, 0x30000002u);
  memory.write32(0x2004u, 0x00003000u);
  memory.write32(0x2010u, 0x20000000u);
  memory.write32(0x2014u, 0x00002100u);
  memory.write32(0x2100u, 0x00000002u);
  memory.write32(0x2104u, 0x00003040u);

  memory.write32(0x3000u, 0x00001000u);
  memory.write32(0x3004u, 4u);
  memory.write32(0x3010u, 0x11111111u);
  memory.write32(0x3014u, 0x22222222u);
  memory.write32(0x3018u, 0x33333333u);
  memory.write32(0x301Cu, 0x44444444u);
  memory.write32(0x3040u, 0xC0001100u);
  memory.write32(0x3044u, 4u);
  memory.write32(0x3050u, 0x55555555u);
  memory.write32(0x3054u, 0x66666666u);
  memory.write32(0x3058u, 0x77777777u);
  memory.write32(0x305Cu, 0x88888888u);

  memory.write32(0x1000C430u, 0x2000u);
  memory.write32(0x1000C400u, 0x184u);
  memory.iop_write32(0x1F801538u, 0x41000300u);
  memory.advance(1u);
  memory.advance(32u);
  check(memory.iop_read32(0x1000u) == 0x11111111u &&
        memory.iop_read32(0x100Cu) == 0x44444444u &&
        memory.iop_read32(0x1100u) == 0x55555555u &&
        memory.iop_read32(0x110Cu) == 0x88888888u,
        "SIF1 zero-length NEXT jumps to and executes the following REFE packet");
  check((memory.read32(0x1000C400u) & 0x100u) == 0u &&
        memory.read32(0x1000C430u) == 0x2110u,
        "SIF1 NEXT chain completes at the jumped REFE tag");
}

void test_sif0_dma_reply() {
  ps2vita::Memory memory;
  // Observed BIOS reply: one IOP end tag supplies six words, followed by an
  // EE CNT/IRQ tag requesting two QWC. The remaining two words are padding.
  memory.iop_write32(0x200Cu, 0x80003000u);
  memory.iop_write32(0x2010u, 6u);
  memory.iop_write32(0x2014u, 0x90000002u);
  memory.iop_write32(0x2018u, 0x00004000u);
  memory.iop_write32(0x201Cu, 0u);
  memory.iop_write32(0x2020u, 0u);
  for (std::uint32_t word = 0; word < 6u; ++word)
    memory.iop_write32(0x3000u + word * 4u, 0xA0000000u + word);
  memory.write32(0x1000C000u, 0x184u);
  memory.iop_write32(0x1F80152Cu, 0x200Cu);
  memory.iop_write32(0x1F801528u, 0x01000701u);

  memory.advance(1u);
  memory.advance(47u);
  check(memory.read32(0x4000u) == 0u,
        "scheduled SIF0 reply is not visible before its word deadline");
  memory.advance(1u);
  check(memory.read32(0x4000u) == 0xA0000000u &&
        memory.read32(0x4014u) == 0xA0000005u &&
        memory.read32(0x4018u) == 0u && memory.read32(0x401Cu) == 0u,
        "SIF0 reply copies IOP words and zero-pads the EE quadword");
  check((memory.read32(0x1000C000u) & 0x100u) == 0u &&
        memory.read32(0x1000C010u) == 0x4020u &&
        (memory.read32(0x1000E010u) & (1u << 5)) != 0u,
        "SIF0 completion advances EE MADR and raises DMAC channel 5");
  check((memory.iop_read32(0x1F801528u) & 0x01000000u) == 0u &&
        memory.iop_read32(0x1F801520u) == 0x3018u &&
        memory.iop_read32(0x1F80152Cu) == 0x201Cu &&
        (memory.iop_read32(0x1F801574u) & (1u << 26)) != 0u,
        "SIF0 completion advances IOP DMA state and raises its flag");
  check((memory.iop_read32(0x1F801070u) & (1u << 3)) != 0u,
        "SIF0 completion raises the shared IOP DMA interrupt");
}

void test_sif0_iop_side_completes_first() {
  ps2vita::Memory memory;
  // The retail BIOS sends large packets whose IOP source tag ends while the
  // EE destination chain remains armed for the next packet.
  memory.iop_write32(0x200Cu, 0x80003000u);
  memory.iop_write32(0x2010u, 4u);
  memory.iop_write32(0x2014u, 0x10000001u);
  memory.iop_write32(0x2018u, 0x00004000u);
  for (std::uint32_t word = 0; word < 4u; ++word)
    memory.iop_write32(0x3000u + word * 4u, 0xB0000000u + word);
  memory.write32(0x1000C000u, 0x184u);
  memory.iop_write32(0x1F80152Cu, 0x200Cu);
  memory.iop_write32(0x1F801528u, 0x01000701u);

  memory.advance(1u);
  memory.advance(31u);
  check(memory.read32(0x4000u) == 0u,
        "one-sided SIF0 packet waits for its word deadline");
  memory.advance(1u);
  check(memory.read32(0x4000u) == 0xB0000000u &&
        memory.read32(0x400Cu) == 0xB0000003u,
        "one-sided SIF0 packet reaches EE memory");
  check((memory.iop_read32(0x1F801528u) & 0x01000000u) == 0u &&
        memory.iop_read32(0x1F801520u) == 0x3010u &&
        memory.iop_read32(0x1F80152Cu) == 0x201Cu &&
        (memory.iop_read32(0x1F801574u) & (1u << 26)) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 3)) != 0u,
        "one-sided SIF0 completion retires and interrupts the IOP channel");
  check((memory.read32(0x1000C000u) & 0x100u) != 0u &&
        memory.read32(0x1000C010u) == 0x4010u &&
        (memory.read32(0x1000E010u) & (1u << 5)) == 0u,
        "one-sided SIF0 completion leaves the EE channel armed");
}

void test_iop_timer5_deadlines() {
  ps2vita::Memory memory;
  memory.iop_write32(0x1F8014A8u, 5u);
  memory.iop_write16(0x1F8014A4u, 0x70u);
  check((memory.iop_read16(0x1F8014A4u) & 0x400u) != 0u,
        "IOP Timer 5 halfword mode writes arm the interrupt latch");
  memory.iop_write32(0x1F801074u, 1u << 16);
  memory.iop_write32(0x1F801078u, 1u);
  memory.advance(39u);
  check(memory.iop_read32(0x1F8014A0u) == 4u &&
        (memory.iop_read32(0x1F8014A4u) & 0x1800u) == 0u &&
        !memory.iop_interrupt_pending(),
        "IOP Timer 5 remains quiet before its EE-cycle deadline");
  memory.advance(1u);
  const auto target_mode = memory.iop_read32(0x1F8014A4u);
  check(memory.iop_read32(0x1F8014A0u) == 5u &&
        (target_mode & 0x800u) != 0u &&
        memory.iop_interrupt_pending(),
        "IOP Timer 5 target sets its flag and INTC bit 16 at the deadline");
  const auto acknowledged_mode = memory.iop_read32(0x1F8014A4u);
  check((acknowledged_mode & 0x1800u) == 0u &&
        (acknowledged_mode & 0x400u) != 0u,
        "IOP Timer 5 mode read clears event flags and rearms interrupts");

  memory.iop_write32(0x1F801070u, ~std::uint32_t{1u << 16});
  memory.iop_write32(0x1F8014A4u, 0x60u);
  memory.iop_write32(0x1F8014A0u, 0xFFFFFFFEu);
  memory.advance(16u);
  check(memory.iop_read32(0x1F8014A0u) == 0u &&
        (memory.iop_read32(0x1F8014A4u) & 0x1000u) != 0u &&
        memory.iop_interrupt_pending(),
        "IOP Timer 5 overflow wraps and raises the same interrupt line");
}

void test_cdvd_reset_status() {
  ps2vita::Memory memory;
  check(memory.valid(0x1F402005u) && memory.read8(0x1F402005u) == 0x4Cu,
        "EE physical CDVD window shares the drive-ready register");
  check(memory.iop_read8(0x1F402005u) == 0x4Cu,
        "CDVD reset state reports drive, mechacon, and DEV9 ready");
  check(memory.iop_read8(0x1F40200Au) == 0x01u &&
        memory.iop_read8(0x1F40200Bu) == 0x01u,
        "CDVD reset state exposes an open tray without media");
  check(memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD reset state has no secondary-command result pending");

  memory.iop_write8(0x1F402017u, 0u);
  memory.iop_write8(0x1F402017u, 1u);
  memory.iop_write8(0x1F402017u, 1u);
  memory.iop_write8(0x1F402016u, 0x40u);
  check(memory.iop_read8(0x1F402017u) == 0u &&
        memory.iop_read8(0x1F402018u) == 0u &&
        memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD OpenConfig returns one successful result byte");
  memory.iop_write8(0x1F402016u, 0x41u);
  unsigned config_bytes = 0;
  while ((memory.iop_read8(0x1F402017u) & 0x40u) == 0u) {
    check(memory.iop_read8(0x1F402018u) == 0u,
          "CDVD blank reset configuration reads as zero");
    ++config_bytes;
  }
  check(config_bytes == 16u,
        "CDVD ReadConfig exposes a bounded sixteen-byte result");

  memory.iop_write8(0x1F402017u, 0x00u);
  memory.iop_write8(0x1F402016u, 0x03u);
  const std::array<std::uint8_t, 4> expected_mechacon = {0x03u, 0x06u, 0x02u, 0x00u};
  bool mechacon_matches = memory.iop_read8(0x1F402017u) == 0u;
  for (const auto byte : expected_mechacon)
    mechacon_matches = mechacon_matches && memory.iop_read8(0x1F402018u) == byte;
  check(mechacon_matches && memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD GetMechaVersion returns the four-byte retail response");

  memory.iop_write8(0x1F402016u, 0x15u);
  check(memory.iop_read8(0x1F402017u) == 0u &&
        memory.iop_read8(0x1F402018u) == 0x05u &&
        memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD ForbidDVDP returns the retail completion code");

  memory.iop_write8(0x1F402016u, 0x22u);
  bool wake_time_is_clear = memory.iop_read8(0x1F402017u) == 0u;
  for (unsigned index = 0; index < 10u; ++index)
    wake_time_is_clear = wake_time_is_clear &&
        memory.iop_read8(0x1F402018u) == 0u;
  check(wake_time_is_clear && memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD ReadWakeUpTime returns a clear ten-byte record");

  memory.iop_write8(0x1F402017u, 1u);
  memory.iop_write8(0x1F402016u, 0x24u);
  check(memory.iop_read8(0x1F402017u) == 0u &&
        memory.iop_read8(0x1F402018u) == 0u &&
        memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD RCBypassCtrl acknowledges the requested mode");

  memory.iop_write8(0x1F402016u, 0x36u);
  const std::array<std::uint8_t, 15> expected_region = {
      0u, 0x08u, 0u, 'E', 'E', 'e', 'n', 'g', 'E', 'E', 0u, 0u, 0u, 0u, 0u};
  bool region_matches = memory.iop_read8(0x1F402017u) == 0u;
  for (const auto byte : expected_region)
    region_matches = region_matches && memory.iop_read8(0x1F402018u) == byte;
  check(region_matches && memory.iop_read8(0x1F402017u) == 0x40u,
        "CDVD ReadRegionParams returns a complete European region block");
}

void test_sio2_disconnected_transfer() {
  ps2vita::Memory memory;
  check(memory.iop_read32(0x1F808268u) == 0x000003BCu &&
        memory.iop_read32(0x1F80826Cu) == 0x0001D100u &&
        memory.iop_read32(0x1F808270u) == 0x0000000Fu,
        "SIO2 exposes its reset control and disconnected-device status");
  check(memory.iop_read8(0x1F808264u) == 0xFFu,
        "SIO2 disconnected FIFO reads return idle bus data");
  memory.iop_write32(0x1F808268u, 0xB1u);
  check(memory.iop_read32(0x1F808268u) == 0xB1u &&
        (memory.iop_read32(0x1F80826Cu) & (1u << 12)) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 17)) != 0u,
        "SIO2 start completes the no-device PIO probe and raises IRQ 17");
}

void test_video_vblank_deadlines() {
  ps2vita::Memory memory;
  memory.write32(0x1000F010u, (1u << 2) | (1u << 3));
  memory.iop_write32(0x1F801074u, (1u << 0) | (1u << 11));
  memory.iop_write32(0x1F801078u, 1u);

  memory.advance(4498395u);
  check((memory.read32(0x1000F000u) & 0xCu) == 0u &&
        !memory.iop_interrupt_pending(),
        "VBlank start remains quiet before its NTSC field deadline");
  memory.advance(1u);
  check((memory.read32(0x1000F000u) & (1u << 2)) != 0u &&
        (memory.ee_interrupt_lines() & 0x400u) != 0u &&
        (memory.iop_read32(0x1F801070u) & 1u) != 0u &&
        memory.iop_interrupt_pending(),
        "VBlank start raises the EE and IOP interrupt lines together");

  memory.write32(0x1000F000u, 1u << 2);
  memory.iop_write32(0x1F801070u, ~std::uint32_t{1u});
  memory.advance(421723u);
  check((memory.read32(0x1000F000u) & (1u << 3)) == 0u &&
        !memory.iop_interrupt_pending(),
        "VBlank end remains quiet before the blanking deadline");
  memory.advance(1u);
  check((memory.read32(0x1000F000u) & (1u << 3)) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 11)) != 0u &&
        memory.iop_interrupt_pending(),
        "VBlank end raises both interrupt controllers at its deadline");
}

void test_spu2_dma7_completion() {
  ps2vita::Memory memory;
  check(memory.iop_read16(0x1F900744u) == 0x0080u,
        "SPU2 core 1 resets ready");
  memory.iop_write16(0x1F90059Au, 0x0020u);
  for (std::uint32_t byte = 0; byte < 8u; ++byte)
    memory.iop_write8(0x1000u + byte, static_cast<std::uint8_t>(0xA0u + byte));
  memory.iop_write16(0x1F9005A8u, 0u);
  memory.iop_write16(0x1F9005AAu, 0x2808u);
  memory.iop_write32(0x1F801500u, 0x1000u);
  memory.iop_write32(0x1F801504u, 0x00010002u);
  memory.iop_write32(0x1F801508u, 0x01000201u);
  check(memory.iop_read16(0x1F900744u) == 0x0400u,
        "SPU2 DMA7 clears ready and sets busy while active");
  memory.iop_write32(0x1F801074u, 1u << 3);
  memory.iop_write32(0x1F801078u, 1u);

  memory.advance(767u);
  check((memory.iop_read32(0x1F801508u) & 0x01000000u) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 3)) == 0u,
        "SPU2 DMA7 stays active until its 24-IOP-cycle word deadline");
  memory.advance(1u);

  bool payload_matches = true;
  for (std::uint32_t byte = 0; byte < 8u; ++byte)
    payload_matches = payload_matches &&
        memory.spu2_ram_read8(0x5010u + byte) == 0xA0u + byte;
  check(payload_matches && memory.iop_read32(0x1F801500u) == 0x1008u &&
        memory.iop_read32(0x1F801504u) == 0u &&
        (memory.iop_read32(0x1F801508u) & 0x01000000u) == 0u &&
        memory.iop_read16(0x1F9005AAu) == 0x280Cu,
        "SPU2 DMA7 copies IOP data and advances its source and sound addresses");
  check(memory.iop_read16(0x1F900744u) == 0x0080u,
        "SPU2 DMA7 clears busy and restores ready at completion");
  check((memory.iop_read32(0x1F801574u) & (1u << 24)) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 3)) != 0u &&
        memory.iop_interrupt_pending(),
        "SPU2 DMA7 completion raises DICR2 and the shared IOP DMA interrupt");
}

void test_event_horizon_contract() {
  ps2vita::Memory memory;
  check(memory.cycles_until_next_event() == 8u,
        "event horizon starts at the next conservative IOP clock edge");
  memory.advance(3u);
  check(memory.cycles_until_next_event() == 5u,
        "event horizon preserves the fractional IOP clock phase");

  memory.write32(0x1000C400u, 0x100u);
  memory.iop_write32(0x1F801538u, 0x01000000u);
  check(memory.cycles_until_next_event() == 1u,
        "event horizon exposes an armed unscheduled SIF1 start");
}

void test_spu2_dma4_completion() {
  ps2vita::Memory memory;
  check(memory.iop_read16(0x1F900344u) == 0x0080u,
        "SPU2 core 0 resets ready");
  memory.iop_write16(0x1F90019Au, 0x0020u);
  for (std::uint32_t byte = 0; byte < 8u; ++byte)
    memory.iop_write8(0x2000u + byte, static_cast<std::uint8_t>(0xB0u + byte));
  memory.iop_write16(0x1F9001A8u, 0u);
  memory.iop_write16(0x1F9001AAu, 0x0010u);
  memory.iop_write32(0x1F8010C0u, 0x2000u);
  memory.iop_write32(0x1F8010C4u, 0x00010002u);
  memory.iop_write32(0x1F8010C8u, 0x01000201u);
  check(memory.iop_read16(0x1F900344u) == 0x0400u,
        "SPU2 DMA4 clears ready and sets busy while active");

  memory.advance(768u);

  bool payload_matches = true;
  for (std::uint32_t byte = 0; byte < 8u; ++byte)
    payload_matches = payload_matches &&
        memory.spu2_ram_read8(0x20u + byte) == 0xB0u + byte;
  check(payload_matches && memory.iop_read32(0x1F8010C0u) == 0x2008u &&
        memory.iop_read32(0x1F8010C4u) == 0u &&
        (memory.iop_read32(0x1F8010C8u) & 0x01000000u) == 0u &&
        memory.iop_read16(0x1F9001AAu) == 0x0014u,
        "SPU2 DMA4 copies IOP data and advances core-0 transfer state");
  check(memory.iop_read16(0x1F900344u) == 0x0080u,
        "SPU2 DMA4 clears busy and restores ready at completion");
  check((memory.iop_read32(0x1F8010F4u) & (1u << 28)) != 0u &&
        (memory.iop_read32(0x1F801070u) & (1u << 3)) != 0u,
        "SPU2 DMA4 completion raises DICR and the shared IOP DMA interrupt");
}

void test_ee_timer3_hblank_clock() {
  ps2vita::Memory memory;
  memory.write32(0x10001820u, 2u);
  memory.write32(0x10001810u, 0xD83u); // Clear flags, count HBlank, target IRQ.
  check(memory.read32(0x10001810u) == 0x183u,
        "EE Timer 3 mode writes clear reached flags with write-one semantics");
  memory.write32(0x1000F010u, 1u << 12);
  memory.advance(18742u);
  check(memory.read32(0x10001800u) == 0u &&
        (memory.ee_interrupt_lines() & 0x400u) == 0u,
        "EE Timer 3 remains quiet before the HBlank edge");
  memory.advance(1u);
  check(memory.read32(0x10001800u) == 1u,
        "EE Timer 3 advances from its external HBlank clock");
  memory.advance(18743u);
  check(memory.read32(0x10001800u) == 2u &&
        (memory.read32(0x10001810u) & 0x400u) != 0u &&
        (memory.ee_interrupt_lines() & 0x400u) != 0u,
        "EE Timer 3 target raises INTC bit 12 on the exact scanline");
}

void test_exception_entry_and_eret() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  std::vector<std::uint8_t> bios(ps2vita::Memory::kBiosSize, 0);
  const std::uint32_t eret = 0x42000018u;
  bios[0x200] = static_cast<std::uint8_t>(eret);
  bios[0x201] = static_cast<std::uint8_t>(eret >> 8);
  bios[0x202] = static_cast<std::uint8_t>(eret >> 16);
  bios[0x203] = static_cast<std::uint8_t>(eret >> 24);
  check(memory.load_bios(bios.data(), bios.size()), "exception test BIOS mapping");
  memory.write32(0x1000, 0x0000000C); // syscall
  cpu.reset(0x1000);
  cpu.state().cop0[12] = 1u << 22;
  cpu.set_exception_mode(true);
  check(cpu.step() == ps2vita::StopReason::None, "syscall enters exception handler");
  check(cpu.state().pc == 0xBFC00200, "bootstrap exception vector");
  check(((cpu.state().cop0[13] >> 2) & 31u) == 8, "syscall cause code");
  check(cpu.state().cop0[14] == 0x1000, "exception EPC");
  check((cpu.state().cop0[12] & 2u) != 0, "exception sets EXL");
  check(cpu.step() == ps2vita::StopReason::None, "ERET executes");
  check(cpu.state().pc == 0x1000 && (cpu.state().cop0[12] & 2u) == 0,
        "ERET restores EPC and clears EXL");
}

void test_cop0_count_advances() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  memory.write32(0x1000, 0x40084800u); // mfc0 t0,Count
  memory.write32(0x1004, 0);
  memory.write32(0x1008, 0);
  memory.write32(0x100C, 0x40094800u); // mfc0 t1,Count
  memory.write32(0x1010, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(8) == ps2vita::StopReason::Break, "COP0 Count test executes");
  check(cpu.state().gpr[9] > cpu.state().gpr[8], "COP0 Count follows EE cycles");
}

void test_di_ei_status() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  memory.write32(0x1000, 0x42000039u); // DI
  memory.write32(0x1004, 0x42000038u); // EI
  cpu.state().pc = 0x1000;
  cpu.state().cop0[12] = 0x00030001u; // EDI, EIE, IE
  check(cpu.step() == ps2vita::StopReason::None &&
        (cpu.state().cop0[12] & 0x00010000u) == 0u &&
        (cpu.state().cop0[12] & 1u) != 0u,
        "DI clears R5900 EIE without changing IE");
  check(cpu.step() == ps2vita::StopReason::None &&
        (cpu.state().cop0[12] & 0x00010000u) != 0u,
        "EI sets R5900 EIE");

  cpu.state().pc = 0x1000;
  cpu.state().cop0[12] = 0x00010010u; // EIE set, user KSU, EDI clear
  cpu.step();
  check((cpu.state().cop0[12] & 0x00010000u) != 0u,
        "user-mode DI cannot change EIE without EDI permission");
}

void test_empty_ram_vector_falls_back_to_rom() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  std::vector<std::uint8_t> bios(ps2vita::Memory::kBiosSize, 0);
  check(memory.load_bios(bios.data(), bios.size()), "fallback test BIOS mapping");
  memory.write32(0x1000, 0x0000000C); // syscall
  cpu.reset(0x1000);
  cpu.set_exception_mode(true);
  cpu.state().cop0[12] = 0; // BEV clear, but RAM vector has not been installed.
  check(cpu.step() == ps2vita::StopReason::None, "empty vector exception handled");
  check(cpu.state().pc == 0xBFC00200u,
        "empty RAM exception vector retains bootstrap ROM service");
}

void test_tlb_translation() {
  ps2vita::Memory memory;
  memory.write_tlb(3, 0, 0xC0000000u, (0x00010u << 6) | 7u,
                   (0x00011u << 6) | 7u);
  memory.write32(0x00010130, 0xAABBCCDD);
  check(memory.read32(0xC0000130) == 0xAABBCCDD, "TLB even-page translation");
  memory.write32(0x00011234, 0x12345678);
  check(memory.read32(0xC0001234) == 0x12345678, "TLB odd-page translation");
  check(memory.probe_tlb(0xC0000000u) == 3, "TLBP finds mapped VPN");
  std::uint32_t mask = 0, hi = 0, lo0 = 0, lo1 = 0;
  check(memory.read_tlb(3, mask, hi, lo0, lo1) && hi == 0xC0000000u,
        "TLBR returns entry state");

  memory.write32(0x80000700u, 0xA5A55A5Au);
  memory.write_tlb(0, 0, 0x70000000u, 0x80000007u, 0x00000007u);
  for (unsigned page = 0; page < 4; ++page) {
    const auto address = 0x70000080u + page * 0x1000u;
    memory.write32(address, 0x51000000u + page);
    check(memory.read32(address) == 0x51000000u + page,
          "scratchpad TLB entry covers each 4 KiB quarter");
  }
  memory.write32(0x70001700u, 0);
  check(memory.read32(0x80000700u) == 0xA5A55A5Au,
        "scratchpad second quarter does not alias physical low RAM");
}

void test_ee_internal_registers() {
  ps2vita::Memory memory;
  constexpr std::uint32_t cache_control = 0xFFFE0130u;
  check(memory.valid(cache_control, 4), "EE internal control page is mapped");
  memory.write32(cache_control, 0x00000CC4u);
  check(memory.read32(cache_control) == 0x00000CC4u,
        "EE internal cache-control register retains its value");
}

void test_absent_dev_board_window() {
  ps2vita::Memory memory;
  check(memory.valid(ps2vita::Memory::kDevBoardBase),
        "optional development-board aperture is handled");
  memory.write8(ps2vita::Memory::kDevBoardBase, 0x5A);
  check(memory.read8(ps2vita::Memory::kDevBoardBase) == 0,
        "absent development board behaves as a null device");
}

void test_cpu_delay_slot() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  const std::uint32_t base = 0x1000;
  memory.write32(base + 0x00, i_type(0x09, 0, 8, 5));       // addiu t0,zero,5
  memory.write32(base + 0x04, i_type(0x04, 8, 8, 2));       // beq t0,t0,+2
  memory.write32(base + 0x08, i_type(0x09, 0, 9, 7));       // delay: addiu t1,zero,7
  memory.write32(base + 0x0C, i_type(0x09, 0, 9, 99));      // skipped
  memory.write32(base + 0x10, i_type(0x2B, 0, 9, 0x2000));  // sw t1,0x2000
  memory.write32(base + 0x14, 0x0000000D);                  // break
  cpu.reset(base);
  check(cpu.run(20) == ps2vita::StopReason::Break, "CPU reaches break");
  check(memory.read32(0x2000) == 7, "branch executes one delay slot");
  check(cpu.state().gpr[0] == 0, "zero register remains immutable");
}

void test_interrupt_waits_for_branch_delay_slot() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  constexpr std::uint32_t base = 0x1000u;
  memory.write32(base + 0x00, i_type(0x04, 0, 0, 3));  // beq zero,zero,0x1010
  memory.write32(base + 0x04, i_type(0x09, 0, 8, 7));  // delay: addiu t0,zero,7
  memory.write32(base + 0x08, 0x0000000Cu);             // unreachable syscall
  memory.write32(base + 0x10, i_type(0x09, 0, 9, 9));  // branch target
  memory.write32(0x80000200u, i_type(0x09, 0, 10, 1)); // installed interrupt vector

  cpu.reset(base);
  cpu.set_exception_mode(true);
  cpu.state().cop0[12] = 0x00010401u; // EIE, INTC mask, IE.
  check(cpu.step() == ps2vita::StopReason::None && cpu.state().pc == base + 4u,
        "branch reaches its delay slot before the interrupt");

  // Raise INTC bit 12 from Timer 3 after the branch has been decoded but
  // before its delay-slot instruction retires.
  memory.write32(0x10001820u, 1u);
  memory.write32(0x10001810u, 0xD83u);
  memory.write32(0x1000F010u, 1u << 12);
  memory.advance(18743u);
  check(memory.ee_interrupt_lines() == 0x400u,
        "test interrupt becomes pending in the branch delay slot");

  check(cpu.step() == ps2vita::StopReason::None &&
        cpu.state().pc == base + 0x10u && cpu.state().gpr[8] == 7u,
        "pending interrupt does not discard the branch delay slot or target");
  check(cpu.step() == ps2vita::StopReason::None &&
        cpu.state().pc == 0x80000200u && cpu.state().cop0[14] == base + 0x10u,
        "interrupt is taken at the completed branch target");
  check((cpu.state().cop0[13] & 0x80000000u) == 0u &&
        cpu.state().gpr[9] == 0u,
        "deferred asynchronous interrupt has no delay-slot cause flag");
}

void test_unaligned_memory_ops() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  memory.write32(0x3000, 0x44332211);
  cpu.state().gpr[8] = 0xFFFFFFFFAABBCCDDull;
  memory.write32(0x1000, i_type(0x22, 0, 8, 0x3001)); // lwl t0,0x3001(zero)
  memory.write32(0x1004, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.step() == ps2vita::StopReason::None, "LWL executes");
  check(cpu.state().gpr[8] == 0x000000002211CCDDull, "LWL little-endian merge");

  cpu.reset(0x1100);
  cpu.state().gpr[8] = 0x12345678AABBCCDDull;
  memory.write32(0x1100, i_type(0x26, 0, 8, 0x3001)); // lwr t0,0x3001(zero)
  check(cpu.step() == ps2vita::StopReason::None, "LWR executes");
  check(cpu.state().gpr[8] == 0x12345678AA443322ull, "LWR little-endian merge preserves upper word");

  cpu.reset(0x1200);
  cpu.state().gpr[8] = 0xAABBCCDD;
  memory.write32(0x3000, 0x44332211);
  memory.write32(0x1200, i_type(0x2A, 0, 8, 0x3001)); // swl
  check(cpu.step() == ps2vita::StopReason::None, "SWL executes");
  check(memory.read32(0x3000) == 0x4433AABB, "SWL little-endian merge");

  cpu.reset(0x1300);
  cpu.state().gpr[8] = 0xAABBCCDD;
  memory.write32(0x3000, 0x44332211);
  memory.write32(0x1300, i_type(0x2E, 0, 8, 0x3001)); // swr
  check(cpu.step() == ps2vita::StopReason::None, "SWR executes");
  check(memory.read32(0x3000) == 0xBBCCDD11, "SWR little-endian merge");
}

void test_quadword_load_store() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  memory.write64(0x4000, 0x0123456789ABCDEFull);
  memory.write64(0x4008, 0xFEDCBA9876543210ull);
  memory.write32(0x1000, i_type(0x1E, 0, 8, 0x4007)); // lq t0, unaligned address
  memory.write32(0x1004, i_type(0x1F, 0, 8, 0x401F)); // sq t0, unaligned address
  memory.write32(0x1008, 0x0000000D);
  cpu.reset(0x1000);
  check(cpu.run(10) == ps2vita::StopReason::Break, "LQ/SQ program executes");
  check(cpu.state().gpr[8] == 0x0123456789ABCDEFull, "LQ low 64 bits");
  check(cpu.state().gpr_hi[8] == 0xFEDCBA9876543210ull, "LQ high 64 bits");
  check(memory.read64(0x4010) == 0x0123456789ABCDEFull, "SQ low 64 bits");
  check(memory.read64(0x4018) == 0xFEDCBA9876543210ull, "SQ high 64 bits");
}

void test_compiler_support_ops() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 0x1234;
  cpu.state().gpr[9] = 0;
  cpu.state().gpr[10] = 1;
  // movz s0,t0,t1; movn s1,t0,t2; dsllv s2,t0,t2; sync; cache; pref; break
  memory.write32(0x1000, (8u << 21) | (9u << 16) | (16u << 11) | 0x0Au);
  memory.write32(0x1004, (8u << 21) | (10u << 16) | (17u << 11) | 0x0Bu);
  memory.write32(0x1008, (10u << 21) | (8u << 16) | (18u << 11) | 0x14u);
  memory.write32(0x100C, 0x0000000F);
  memory.write32(0x1010, i_type(0x2F, 0, 0, 0));
  memory.write32(0x1014, i_type(0x33, 0, 0, 0));
  memory.write32(0x1018, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(20) == ps2vita::StopReason::Break, "compiler support op sequence");
  check(cpu.state().gpr[16] == 0x1234, "MOVZ");
  check(cpu.state().gpr[17] == 0x1234, "MOVN");
  check(cpu.state().gpr[18] == 0x2468, "DSLLV");
}

void test_doubleword_arithmetic() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 0x100000001ull;
  cpu.state().gpr[9] = 0x200000002ull;
  memory.write32(0x1000, (8u << 21) | (9u << 16) | (10u << 11) | 0x2Cu);
  memory.write32(0x1004, (9u << 21) | (8u << 16) | (11u << 11) | 0x2Eu);
  memory.write32(0x1008, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(10) == ps2vita::StopReason::Break,
        "DADD/DSUB sequence executes");
  check(cpu.state().gpr[10] == 0x300000003ull, "DADD keeps 64-bit result");
  check(cpu.state().gpr[11] == 0x100000001ull, "DSUB keeps 64-bit result");
}

void test_decoded_block_cache() {
  ps2vita::Memory memory;
  ps2vita::EeBlockCache cache;
  memory.write32(0x1000, i_type(0x04, 0, 0, 1)); // beq zero,zero,+1
  memory.write32(0x1004, i_type(0x09, 0, 8, 1)); // delay: addiu t0,zero,1
  memory.write32(0x1008, i_type(0x09, 0, 8, 2));
  const auto& first = cache.lookup(memory, 0x1000);
  check(first.valid && first.instruction_count == 2,
        "decoded block includes branch delay slot and then stops");
  check((first.instructions[1].flags & ps2vita::EeDelaySlot) != 0,
        "decoded block marks delay slot");

  memory.write32(0x1004, i_type(0x09, 0, 8, 7));
  const auto& refreshed = cache.lookup(memory, 0x1000);
  check(refreshed.instructions[1].opcode == i_type(0x09, 0, 8, 7),
        "guest RAM write invalidates decoded block by page generation");
}

void test_mmi_padduw() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 0xFFFFFFFF00000001ull;
  cpu.state().gpr_hi[8] = 0x80000000FFFFFFFEull;
  cpu.state().gpr[9] = 0x0000000200000002ull;
  cpu.state().gpr_hi[9] = 0x8000000000000003ull;
  const std::uint32_t padduw = (0x1Cu << 26) | (8u << 21) | (9u << 16) |
                               (10u << 11) | (0x10u << 6) | 0x28u;
  memory.write32(0x1000, padduw);
  memory.write32(0x1004, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PADDUW executes");
  check(cpu.state().gpr[10] == 0xFFFFFFFF00000003ull,
        "PADDUW saturates low packed words");
  check(cpu.state().gpr_hi[10] == 0xFFFFFFFFFFFFFFFFull,
        "PADDUW saturates high packed words");
}

void test_mmi_por() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 0x00FF00FF00FF00FFull;
  cpu.state().gpr_hi[8] = 0xAAAAAAAAAAAAAAAAull;
  cpu.state().gpr[9] = 0xFF00FF00FF00FF00ull;
  cpu.state().gpr_hi[9] = 0x5555555555555555ull;
  memory.write32(0x1000,
      (0x1Cu << 26) | (8u << 21) | (9u << 16) | (10u << 11) |
      (0x12u << 6) | 0x29u); // por t2,t0,t1
  memory.write32(0x1004, 0x0000000Du);
  cpu.state().pc = 0x1000;
  check(cpu.run(4) == ps2vita::StopReason::Break, "POR executes");
  check(cpu.state().gpr[10] == 0xFFFFFFFFFFFFFFFFull &&
        cpu.state().gpr_hi[10] == 0xFFFFFFFFFFFFFFFFull,
        "POR combines all 128 register bits");
}

void test_mmi_plzcw() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[1] = 0xFFF000000000FFFFull;
  cpu.state().gpr_hi[26] = 0x0123456789ABCDEFull;
  memory.write32(0x1000u,
      (0x1Cu << 26) | (1u << 21) | (26u << 11) | 0x04u);
  memory.write32(0x1004u, 0x0000000Du);
  cpu.state().pc = 0x1000u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PLZCW executes");
  check(cpu.state().gpr[26] == 0x0000000B0000000Full &&
        cpu.state().gpr_hi[26] == 0x0123456789ABCDEFull,
        "PLZCW counts leading sign bits in both low words only");
}

void test_native_zero_fill_fast_path() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  constexpr std::uint32_t code = 0x1000u;
  constexpr unsigned cursor = 16;
  constexpr unsigned end = 4;
  constexpr unsigned value = 2;
  constexpr unsigned condition = 2;
  memory.write32(code + 0u, i_type(0x1F, cursor, value, 0));
  memory.write32(code + 4u, i_type(0x09, cursor, cursor, 16));
  memory.write32(code + 8u,
      (cursor << 21) | (end << 16) | (condition << 11) | 0x2Bu);
  memory.write32(code + 12u, 0);
  memory.write32(code + 16u, 0);
  memory.write32(code + 20u, i_type(0x05, condition, 0, 0xFFFAu));
  memory.write32(code + 24u,
      (0x1Cu << 26) | (value << 11) | (0x12u << 6) | 0x29u);
  memory.write32(code + 28u, 0x0000000Du);
  for (std::uint32_t address = 0x4000u; address < 0x4100u; address += 8u)
    memory.write64(address, 0xFFFFFFFFFFFFFFFFull);
  cpu.reset(code);
  cpu.state().gpr[cursor] = 0x4000u;
  cpu.state().gpr[end] = 0x4100u;

  check(cpu.run(14) == ps2vita::StopReason::StepLimit,
        "native zero-fill respects a partial guest budget");
  check(cpu.state().pc == code && cpu.state().gpr[cursor] == 0x4020u &&
        memory.read64(0x4000u) == 0 && memory.read64(0x4018u) == 0 &&
        memory.read64(0x4020u) == 0xFFFFFFFFFFFFFFFFull,
        "native zero-fill resumes at the loop boundary");
  check(cpu.run(200) == ps2vita::StopReason::Break,
        "native zero-fill reaches the following instruction");
  check(cpu.state().pc == code + 28u && cpu.state().gpr[cursor] == 0x4100u &&
        cpu.state().gpr[condition] == 0 &&
        memory.read64(0x40F8u) == 0,
        "native zero-fill preserves final EE state and clears RAM");
  check(cpu.state().fast_path_instructions == 16u * 7u,
        "native zero-fill accounts optimized guest instructions");
}

void test_mmi_div1_accumulator() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 100;
  cpu.state().gpr[9] = 7;
  memory.write32(0x1000, (0x1Cu << 26) | (8u << 21) | (9u << 16) | 0x1Au);
  memory.write32(0x1004, (0x1Cu << 26) | (10u << 11) | 0x12u); // mflo1 t2
  memory.write32(0x1008, (0x1Cu << 26) | (11u << 11) | 0x10u); // mfhi1 t3
  memory.write32(0x100C, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(8) == ps2vita::StopReason::Break, "DIV1/MFLO1/MFHI1 execute");
  check(cpu.state().gpr[10] == 14 && cpu.state().gpr[11] == 2,
        "DIV1 exposes quotient and remainder through LO1/HI1");
}

void test_mmi_packed_accumulator_moves() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().hi = 0x1111222233334444ull;
  cpu.state().hi1 = 0x5555666677778888ull;
  cpu.state().lo = 0x9999AAAABBBBCCCCull;
  cpu.state().lo1 = 0xDDDDEEEEFFFF0000ull;
  cpu.state().gpr[4] = 0x0123456789ABCDEFull;
  cpu.state().gpr_hi[4] = 0xFEDCBA9876543210ull;
  const auto mmi_group = [](unsigned fn, unsigned sub, unsigned rs,
                            unsigned rd) {
    return (0x1Cu << 26) | (rs << 21) | (rd << 11) | (sub << 6) | fn;
  };
  memory.write32(0x1000u, mmi_group(0x09, 0x08, 0, 2)); // pmfhi v0
  memory.write32(0x1004u, mmi_group(0x09, 0x09, 0, 3)); // pmflo v1
  memory.write32(0x1008u, mmi_group(0x29, 0x08, 4, 0)); // pmthi a0
  memory.write32(0x100Cu, mmi_group(0x29, 0x09, 4, 0)); // pmtlo a0
  memory.write32(0x1010u, 0x0000000Du);
  cpu.state().pc = 0x1000u;
  check(cpu.run(8) == ps2vita::StopReason::Break,
        "packed HI/LO accumulator moves execute");
  check(cpu.state().gpr[2] == 0x1111222233334444ull &&
        cpu.state().gpr_hi[2] == 0x5555666677778888ull &&
        cpu.state().gpr[3] == 0x9999AAAABBBBCCCCull &&
        cpu.state().gpr_hi[3] == 0xDDDDEEEEFFFF0000ull,
        "PMFHI/PMFLO preserve all 128 accumulator bits");
  check(cpu.state().hi == cpu.state().gpr[4] &&
        cpu.state().hi1 == cpu.state().gpr_hi[4] &&
        cpu.state().lo == cpu.state().gpr[4] &&
        cpu.state().lo1 == cpu.state().gpr_hi[4],
        "PMTHI/PMTLO restore all 128 accumulator bits");
}

void test_mmi_pcpyld() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[2] = 0x1111222233334444ull;
  cpu.state().gpr[3] = 0xAAAABBBBCCCCDDDDull;
  // pcpyld v1,v1,v0: destination aliases rs, as in interrupt-save code.
  memory.write32(0x1000u, (0x1Cu << 26) | (3u << 21) | (2u << 16) |
                                (3u << 11) | (0x0Eu << 6) | 0x09u);
  memory.write32(0x1004u, 0x0000000Du);
  cpu.state().pc = 0x1000u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PCPYLD executes");
  check(cpu.state().gpr[3] == 0x1111222233334444ull &&
        cpu.state().gpr_hi[3] == 0xAAAABBBBCCCCDDDDull,
        "PCPYLD packs Rt low below Rs low with alias-safe reads");

  cpu.state().gpr_hi[2] = 0x0123456789ABCDEFull;
  cpu.state().gpr_hi[3] = 0xFEDCBA9876543210ull;
  // pcpyud v0,v0,v1: destination aliases rs.
  memory.write32(0x1008u, (0x1Cu << 26) | (2u << 21) | (3u << 16) |
                                (2u << 11) | (0x0Eu << 6) | 0x29u);
  memory.write32(0x100Cu, 0x0000000Du);
  cpu.state().pc = 0x1008u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PCPYUD executes");
  check(cpu.state().gpr[2] == 0x0123456789ABCDEFull &&
        cpu.state().gpr_hi[2] == 0xFEDCBA9876543210ull,
        "PCPYUD packs Rs high below Rt high with alias-safe reads");

  cpu.state().gpr[2] = 0x111122223333ABCDull;
  cpu.state().gpr_hi[2] = 0x9999AAAABBBB7654ull;
  // pcpyh v0,zero,v0: destination aliases rt, as in relocated BIOS code.
  memory.write32(0x1010u, (0x1Cu << 26) | (2u << 16) | (2u << 11) |
                                (0x1Bu << 6) | 0x29u);
  memory.write32(0x1014u, 0x0000000Du);
  cpu.state().pc = 0x1010u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PCPYH executes");
  check(cpu.state().gpr[2] == 0xABCDABCDABCDABCDull &&
        cpu.state().gpr_hi[2] == 0x7654765476547654ull,
        "PCPYH replicates each doubleword's low halfword alias-safely");

  cpu.state().gpr[2] = 0xFFFF0000AAAA5555ull;
  cpu.state().gpr_hi[2] = 0x0123456789ABCDEFull;
  cpu.state().gpr[9] = 0x00FFFF00A5A55A5Aull;
  cpu.state().gpr_hi[9] = 0xFEDCBA9876543210ull;
  // pxor v0,v0,t1: destination aliases rs, as in relocated BIOS code.
  memory.write32(0x1018u, (0x1Cu << 26) | (2u << 21) | (9u << 16) |
                                (2u << 11) | (0x13u << 6) | 0x09u);
  memory.write32(0x101Cu, 0x0000000Du);
  cpu.state().pc = 0x1018u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PXOR executes");
  check(cpu.state().gpr[2] == 0xFF00FF000F0F0F0Full &&
        cpu.state().gpr_hi[2] == 0xFFFFFFFFFFFFFFFFull,
        "PXOR combines all 128 register bits alias-safely");

  cpu.state().gpr[2] = 0x0001020304050607ull;
  cpu.state().gpr_hi[2] = 0x1011121314151617ull;
  cpu.state().gpr[3] = 0x0101010101010101ull;
  cpu.state().gpr_hi[3] = 0x0808080808080808ull;
  memory.write32(0x1020u, (0x1Cu << 26) | (2u << 21) | (3u << 16) |
                                (2u << 11) | (0x09u << 6) | 0x08u);
  memory.write32(0x1024u, 0x0000000Du);
  cpu.state().pc = 0x1020u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PSUBB executes");
  check(cpu.state().gpr[2] == 0xFF00010203040506ull &&
        cpu.state().gpr_hi[2] == 0x08090A0B0C0D0E0Full,
        "PSUBB wraps eight independent byte lanes per register half");

  cpu.state().gpr[2] = 0x0000000010000000ull;
  cpu.state().gpr_hi[2] = 0x8000000000000000ull;
  cpu.state().gpr[3] = 0x0000000120000000ull;
  cpu.state().gpr_hi[3] = 0x00000001FFFFFFFFull;
  memory.write32(0x1028u, 0x70433848u); // psubw a3,v0,v1
  memory.write32(0x102Cu, 0x0000000Du);
  cpu.state().pc = 0x1028u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "captured BIOS PSUBW executes");
  check(cpu.state().gpr[7] == 0xFFFFFFFFF0000000ull &&
        cpu.state().gpr_hi[7] == 0x7FFFFFFF00000001ull,
        "PSUBW wraps four independent 32-bit lanes");

  cpu.state().gpr[2] = 0xFF00FF00FF00FF00ull;
  cpu.state().gpr_hi[2] = 0xAAAAAAAAAAAAAAAAull;
  cpu.state().gpr[3] = 0x0F0F0F0F0F0F0F0Full;
  cpu.state().gpr_hi[3] = 0xCCCCCCCCCCCCCCCCull;
  cpu.state().gpr[5] = cpu.state().gpr[2];
  cpu.state().gpr_hi[5] = cpu.state().gpr_hi[2];
  memory.write32(0x1030u, (0x1Cu << 26) | (2u << 21) | (3u << 16) |
                                (2u << 11) | (0x12u << 6) | 0x09u);
  memory.write32(0x1034u, (0x1Cu << 26) | (5u << 21) | (3u << 16) |
                                (4u << 11) | (0x13u << 6) | 0x29u);
  memory.write32(0x1038u, 0x0000000Du);
  cpu.state().pc = 0x1030u;
  check(cpu.run(8) == ps2vita::StopReason::Break, "PAND and PNOR execute");
  check(cpu.state().gpr[2] == 0x0F000F000F000F00ull &&
        cpu.state().gpr_hi[2] == 0x8888888888888888ull &&
        cpu.state().gpr[4] == 0x00F000F000F000F0ull &&
        cpu.state().gpr_hi[4] == 0x1111111111111111ull,
        "PAND and PNOR combine all 128 bits with alias-safe sources");
}

void test_mmi_pextlw() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[2] = 0x2222222211111111ull;
  cpu.state().gpr[3] = 0xBBBBBBBBAAAAAAAAull;
  // pextlw v0,v1,v0: destination aliases rt, as in the BIOS save path.
  memory.write32(0x1000u, (0x1Cu << 26) | (3u << 21) | (2u << 16) |
                                (2u << 11) | (0x12u << 6) | 0x08u);
  memory.write32(0x1004u, 0x0000000Du);
  cpu.state().pc = 0x1000u;
  check(cpu.run(4) == ps2vita::StopReason::Break, "PExtLW executes");
  check(cpu.state().gpr[2] == 0xAAAAAAAA11111111ull &&
        cpu.state().gpr_hi[2] == 0xBBBBBBBB22222222ull,
        "PExtLW interleaves low words with alias-safe source reads");
}

void test_r5900_shift_amount_moves() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().shift_amount = 0x89ABCDEFu;
  cpu.state().gpr[4] = 0xFEDCBA9876543210ull;
  memory.write32(0x1000u, (2u << 11) | 0x28u);          // mfsa v0
  memory.write32(0x1004u, (4u << 21) | 0x29u);          // mtsa a0
  memory.write32(0x1008u, (3u << 11) | 0x28u);          // mfsa v1
  memory.write32(0x100Cu, 0x0000000Du);
  cpu.state().pc = 0x1000u;
  check(cpu.run(8) == ps2vita::StopReason::Break, "MFSA/MTSA execute");
  check(cpu.state().gpr[2] == 0x89ABCDEFu &&
        cpu.state().shift_amount == 0x76543210u &&
        cpu.state().gpr[3] == 0x76543210u,
        "MFSA zero-extends and MTSA retains the source low word");
}

void test_r5900_three_operand_multiply() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[17] = 159;
  cpu.state().gpr[3] = 0x3D76;
  memory.write32(0x1000, 0x02238818u); // mult s1,v1,s1 (R5900 rd result)
  memory.write32(0x1004, 0x0000000Du);
  cpu.state().pc = 0x1000;
  check(cpu.run(4) == ps2vita::StopReason::Break,
        "R5900 three-operand MULT executes");
  check(cpu.state().gpr[17] == 159u * 0x3D76u &&
        cpu.state().lo == 159u * 0x3D76u,
        "R5900 MULT writes both rd and LO");
}

void test_scalar_fpu() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 0x3FC00000; // 1.5f
  cpu.state().gpr[9] = 0x40100000; // 2.25f
  const auto cop1 = [](unsigned fmt, unsigned ft, unsigned fs, unsigned fd, unsigned fn) {
    return (0x11u << 26) | (fmt << 21) | (ft << 16) | (fs << 11) | (fd << 6) | fn;
  };
  memory.write32(0x1000, cop1(0x04, 8, 0, 0, 0));  // mtc1 t0,f0
  memory.write32(0x1004, cop1(0x04, 9, 1, 0, 0));  // mtc1 t1,f1
  memory.write32(0x1008, cop1(0x10, 1, 0, 2, 0));  // add.s f2,f0,f1
  memory.write32(0x100C, cop1(0x00, 10, 2, 0, 0)); // mfc1 t2,f2
  memory.write32(0x1010, cop1(0x10, 1, 0, 0, 0x18)); // adda.s f0,f1
  memory.write32(0x1014, cop1(0x10, 1, 0, 3, 0x1C)); // madd.s f3,f0,f1
  memory.write32(0x1018, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(10) == ps2vita::StopReason::Break, "scalar COP1 sequence");
  check(cpu.state().gpr[10] == 0x40700000, "ADD.S produces 3.75");
  check(cpu.state().fpu_acc == 0x40700000u &&
        cpu.state().fpr[3] == 0x40E40000u,
        "FPU accumulator feeds MADD.S");

  cpu.reset(0x1020u);
  cpu.state().fpr[0] = 0x3F800000u;
  cpu.state().fpr[5] = 0x40000000u;
  memory.write32(0x1020u, 0x46050034u); // c.olt.s f0,f5
  memory.write32(0x1024u, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break &&
        (cpu.state().fcr[31] & (1u << 23)) != 0u,
        "captured BIOS C.OLT.S sets the ordered less-than condition");

  cpu.reset(0x1030u);
  cpu.state().fpr[4] = 0x40000000u;
  cpu.state().fpr[3] = 0x40000000u;
  memory.write32(0x1030u, 0x46032036u); // c.ole.s f4,f3
  memory.write32(0x1034u, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break &&
        (cpu.state().fcr[31] & (1u << 23)) != 0u,
        "captured BIOS C.OLE.S sets the ordered less-or-equal condition");
}

void test_fpu_memory_transfer() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.state().gpr[8] = 0x2000;
  cpu.state().fpr[3] = 0x40490FDBu;
  memory.write32(0x1000, i_type(0x39, 8, 3, 4)); // swc1 f3,4(t0)
  memory.write32(0x1004, i_type(0x31, 8, 4, 4)); // lwc1 f4,4(t0)
  memory.write32(0x1008, 0x0000000D);
  cpu.state().pc = 0x1000;
  check(cpu.run(6) == ps2vita::StopReason::Break, "LWC1/SWC1 execute");
  check(memory.read32(0x2004) == 0x40490FDBu &&
        cpu.state().fpr[4] == 0x40490FDBu,
        "LWC1/SWC1 preserve raw floating-point bits");
}

void test_vu_memory_windows() {
  ps2vita::Memory memory;
  const std::array<std::uint32_t, 4> banks = {
      ps2vita::Memory::kVu0MicroBase, ps2vita::Memory::kVu0DataBase,
      ps2vita::Memory::kVu1MicroBase, ps2vita::Memory::kVu1DataBase};
  for (unsigned i = 0; i < banks.size(); ++i) {
    check(memory.valid(banks[i], 16), "VU memory bank is EE-visible");
    memory.write32(banks[i], 0xA0B0C000u + i);
    check(memory.read32(banks[i]) == 0xA0B0C000u + i,
          "VU memory bank preserves writes");
  }
  check(!memory.valid(0x11001000u, 4) && !memory.valid(0x11005000u, 4),
        "VU address-map holes remain unmapped");
}

void test_vu0_cop2_transfers() {
  ps2vita::Memory memory;
  ps2vita::Cpu cpu(memory);
  cpu.reset(0x1000);
  cpu.state().gpr[8] = 0xFEDCBA9876543210ull;
  cpu.state().gpr_hi[8] = 0x0123456789ABCDEFull;
  cpu.state().gpr[9] = 0x00000C0Cu;
  const auto cop2 = [](unsigned rs, unsigned rt, unsigned rd) {
    return (0x12u << 26) | (rs << 21) | (rt << 16) | (rd << 11);
  };
  memory.write32(0x1000, cop2(0x04, 8, 3)); // qmtc2 t0,vf3
  memory.write32(0x1004, cop2(0x01, 10, 3)); // qmfc2.i t2,vf3
  memory.write32(0x1008, cop2(0x06, 9, 28)); // ctc2 t1,FBRST
  memory.write32(0x100C, cop2(0x02, 11, 28)); // cfc2 t3,FBRST
  memory.write32(0x1010, 0x0000000Du);
  check(cpu.run(8) == ps2vita::StopReason::Break,
        "COP2 control/vector transfers execute");
  check(cpu.state().gpr[10] == 0xFEDCBA9876543210ull &&
        cpu.state().gpr_hi[10] == 0x0123456789ABCDEFull,
        "QMTC2/QMFC2 preserve all 128 bits");
  check(cpu.state().gpr[11] == 0x00000C0Cu,
        "FBRST masks writable control bits");

  cpu.reset(0x1080);
  cpu.state().vu0_vi[1] = 0x421u;
  cpu.state().vu0_vi[2] = 0xBEEFu;
  memory.write32(ps2vita::Memory::kVu0DataBase + 0x21u * 16u, 0xFFFFFFFFu);
  memory.write32(0x1080, 0x4B020BFFu); // viswr.x vi2,(vi1)
  memory.write32(0x1084, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break, "VISWR executes");
  check(memory.read32(ps2vita::Memory::kVu0DataBase + 0x21u * 16u) ==
            0x0000BEEFu,
        "VISWR wraps VU0 memory and writes a selected integer lane");

  cpu.reset(0x10C0);
  cpu.state().vu0_vf[2] = 0x4080000040400000ull; // 3, 4
  cpu.state().vu0_vf_hi[2] = 0x40C0000040A00000ull; // 5, 6
  cpu.state().vu0_vf[3] = 0x3F8000003F800000ull; // 1, 1
  cpu.state().vu0_vf_hi[3] = 0x3F8000003F800000ull; // 1, 1
  memory.write32(0x10C0, (0x12u << 26) | (0x1Fu << 21) | (3u << 16) |
                              (2u << 11) | (4u << 6) | 0x2Cu);
  memory.write32(0x10C4, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break, "VU0 VSUB executes");
  check(cpu.state().vu0_vf[4] == 0x4040000040000000ull &&
        cpu.state().vu0_vf_hi[4] == 0x40A0000040800000ull,
        "VU0 VSUB updates selected vector lanes");

  cpu.reset(0x10C8);
  cpu.state().vu0_vf[4] = 0x400000003F800000ull;
  cpu.state().vu0_vf_hi[4] = 0x4080000040400000ull;
  memory.write32(0x10C8, 0x4A202128u); // vadd.w vf4,vf4,vf0
  memory.write32(0x10CC, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break,
        "captured BIOS VU0 VADD executes");
  check(cpu.state().vu0_vf[4] == 0x400000003F800000ull &&
        cpu.state().vu0_vf_hi[4] == 0x40A0000040400000ull,
        "VU0 VADD honors its W-only destination mask and VF0 constant");

  cpu.reset(0x10D0);
  cpu.state().vu0_vf[4] = 0x2222222211111111ull;
  cpu.state().vu0_vf_hi[4] = 0x4444444433333333ull;
  memory.write32(0x10D0, 0x4BE5233Du); // vmr32.xyzw vf5,vf4
  memory.write32(0x10D4, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break,
        "captured BIOS VU0 VMR32 executes");
  check(cpu.state().vu0_vf[5] == 0x3333333322222222ull &&
        cpu.state().vu0_vf_hi[5] == 0x1111111144444444ull,
        "VU0 VMR32 rotates XYZW lanes and honors the destination mask");

  cpu.reset(0x10E0);
  cpu.state().vu0_vi[2] = 7;
  cpu.state().vu0_vi[3] = 9;
  memory.write32(0x10E0, (0x12u << 26) | (0x10u << 21) | (3u << 16) |
                              (2u << 11) | (4u << 6) | 0x30u);
  memory.write32(0x10E4, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break &&
        (cpu.state().vu0_vi[4] & 0xFFFFu) == 16u,
        "VU0 VIADD writes a 16-bit integer register result");

  cpu.reset(0x10F0);
  cpu.state().vu0_vi[1] = 0x101u;
  cpu.state().vu0_vf[2] = 0x0123456789ABCDEFull;
  cpu.state().vu0_vf_hi[2] = 0xFEDCBA9876543210ull;
  memory.write32(0x10F0, 0x4BE1137Du); // vsqi.xyzw vf2,(vi1++)
  memory.write32(0x10F4, 0x0000000Du);
  check(cpu.run(4) == ps2vita::StopReason::Break, "VU0 VSQI executes");
  check(memory.read32(ps2vita::Memory::kVu0DataBase + 16u) == 0x89ABCDEFu &&
        memory.read32(ps2vita::Memory::kVu0DataBase + 28u) == 0xFEDCBA98u &&
        (cpu.state().vu0_vi[1] & 0xFFFFu) == 0x102u,
        "VU0 VSQI stores lanes, wraps memory, and increments VI");

  cpu.reset(0x1100);
  cpu.state().gpr[8] = 0x3003;
  cpu.state().vu0_vf[4] = 0x1122334455667788ull;
  cpu.state().vu0_vf_hi[4] = 0x99AABBCCDDEEFF00ull;
  memory.write32(0x1100, i_type(0x3E, 8, 4, 5)); // sqc2 vf4,5(t0)
  memory.write32(0x1104, i_type(0x36, 8, 5, 9)); // lqc2 vf5,9(t0)
  memory.write32(0x1108, 0x0000000Du);
  check(cpu.run(6) == ps2vita::StopReason::Break, "LQC2/SQC2 execute");
  check(cpu.state().vu0_vf[5] == 0x1122334455667788ull &&
        cpu.state().vu0_vf_hi[5] == 0x99AABBCCDDEEFF00ull,
        "LQC2/SQC2 align and preserve VU vector bits");
}

void test_quarter_scale_gs() {
  ps2vita::Gs gs;
  gs.clear(0xFF000000u);
  gs.triangle({10, 10, 100, 0xFF0000FFu},
              {150, 10, 100, 0xFF00FF00u},
              {80, 100, 100, 0xFFFF0000u});
  check(gs.pixel(80, 40) != 0xFF000000u, "GS triangle covers interior");
  const auto before = gs.pixel(80, 40);
  gs.point({80, 40, 200, 0xFFFFFFFFu});
  check(gs.pixel(80, 40) == before, "GS rejects farther depth");
  gs.point({80, 40, 50, 0xFFFFFFFFu});
  check(gs.pixel(80, 40) == 0xFFFFFFFFu, "GS accepts nearer depth");
  gs.line({0, 0, 0, 0xFFFFFFFFu}, {159, 111, 0, 0xFFFFFFFFu});
  check(gs.pixel(0, 0) == 0xFFFFFFFFu && gs.pixel(159, 111) == 0xFFFFFFFFu,
        "GS line includes endpoints");
}

void test_gif_normal_dma_completion() {
  ps2vita::Memory memory;
  for (std::uint32_t byte = 0; byte < 32u; ++byte)
    memory.write8(0x2000u + byte, static_cast<std::uint8_t>(0x80u + byte));
  memory.write32(0x1000A010u, 0x2000u);
  memory.write32(0x1000A020u, 2u);
  memory.write32(0x1000A000u, 0x101u);

  memory.advance(1u); // Discover the armed normal-mode transfer.
  memory.advance(15u);
  check((memory.read32(0x1000A000u) & 0x100u) != 0u,
        "GIF DMA remains active before its QWC deadline");
  memory.advance(1u);

  std::vector<std::uint8_t> packet;
  const bool queued = memory.pop_gif_packet(packet);
  bool payload_matches = queued && packet.size() == 32u;
  for (std::uint32_t byte = 0; payload_matches && byte < 32u; ++byte)
    payload_matches = packet[byte] == static_cast<std::uint8_t>(0x80u + byte);
  check(payload_matches, "GIF DMA queues an exact snapshot of its payload");
  check(memory.read32(0x1000A010u) == 0x2020u &&
        memory.read32(0x1000A020u) == 0u &&
        (memory.read32(0x1000A000u) & 0x100u) == 0u &&
        (memory.read32(0x1000E010u) & (1u << 2)) != 0u,
        "GIF DMA retires registers and raises channel 2 completion");
  check(!memory.pop_gif_packet(packet),
        "GIF DMA completion payload is consumed exactly once");
}

void test_vif1_source_chain_completion() {
  ps2vita::Memory memory;
  // CNT with one inline qword, followed by END with one inline qword. TTE
  // contributes each tag's upper 64 bits to the VIF command stream.
  memory.write64(0x2000u, 0x10000001ull);
  memory.write64(0x2008u, 0x1111222233334444ull);
  memory.write64(0x2010u, 0xAAAABBBBCCCCDDDDull);
  memory.write64(0x2018u, 0x0123456789ABCDEFull);
  memory.write64(0x2020u, 0x70000001ull);
  memory.write64(0x2028u, 0x5555666677778888ull);
  memory.write64(0x2030u, 0x1020304050607080ull);
  memory.write64(0x2038u, 0xFFEEDDCCBBAA0099ull);
  memory.write32(0x10009030u, 0x2000u);
  memory.write32(0x10009000u, 0x145u);

  memory.advance(1u);
  memory.advance(15u);
  check((memory.read32(0x10009000u) & 0x100u) != 0u,
        "VIF1 source chain remains active before its QWC deadline");
  memory.advance(1u);
  std::vector<std::uint8_t> packet;
  check(memory.pop_vif1_packet(packet) && packet.size() == 48u,
        "VIF1 source chain queues TTE tag data and inline payloads");
  check(memory.read32(0x10009030u) == 0x2040u &&
        memory.read32(0x10009020u) == 0u &&
        (memory.read32(0x10009000u) & 0x100u) == 0u &&
        (memory.read32(0x1000E010u) & (1u << 1)) != 0u,
        "VIF1 source chain retires and raises DMAC channel 1 completion");
  std::uint64_t first = 0;
  std::uint64_t second_tag = 0;
  std::memcpy(&first, packet.data(), sizeof(first));
  std::memcpy(&second_tag, packet.data() + 24u, sizeof(second_tag));
  check(first == 0x1111222233334444ull &&
        second_tag == 0x5555666677778888ull,
        "VIF1 TTE data is interleaved at each source-chain boundary");
}

void test_vif1_mpg_upload() {
  constexpr std::array<std::uint32_t, 5> words{{
      0x00000000u, 0x01000404u, 0x4A010002u,
      0x89ABCDEFu, 0x01234567u,
  }};
  ps2vita::Memory memory;
  ps2vita::Vif1 vif(memory);
  check(vif.submit(reinterpret_cast<const std::uint8_t*>(words.data()),
                   sizeof(words)) &&
        vif.micro_instructions_loaded() == 1u && vif.cycle() == 0x0404u,
        "VIF1 frontend accepts STCYCL followed by a one-instruction MPG upload");
  check(memory.read32(ps2vita::Memory::kVu1MicroBase + 16u) == 0x89ABCDEFu &&
        memory.read32(ps2vita::Memory::kVu1MicroBase + 20u) == 0x01234567u,
        "VIF1 MPG writes both halves of a VU1 microinstruction");
}

void test_vif1_v4_32_unpack() {
  constexpr std::array<std::uint32_t, 10> words{{
      0x01000404u, 0x6C020001u,
      0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u,
      0xAAAAAAA1u, 0xAAAAAAA2u, 0xAAAAAAA3u, 0xAAAAAAA4u,
  }};
  ps2vita::Memory memory;
  ps2vita::Vif1 vif(memory);
  check(vif.submit(reinterpret_cast<const std::uint8_t*>(words.data()),
                   sizeof(words)) && vif.vectors_unpacked() == 2u,
        "VIF1 frontend accepts contiguous V4-32 UNPACK data");
  check(memory.read32(ps2vita::Memory::kVu1DataBase + 16u) == 0x11111111u &&
        memory.read32(ps2vita::Memory::kVu1DataBase + 44u) == 0xAAAAAAA4u,
        "VIF1 V4-32 UNPACK writes complete vectors at the encoded address");
}

void test_vu1_captured_prologue() {
  ps2vita::Memory memory;
  constexpr std::array<std::uint32_t, 5> lower{{
      0x10010000u, 0x10020004u, 0x10030016u, 0x420F00C5u,
      0x8000033Cu,
  }};
  for (std::size_t index = 0; index < lower.size(); ++index) {
    memory.write32(ps2vita::Memory::kVu1MicroBase +
                   static_cast<std::uint32_t>(index * 8u), lower[index]);
    memory.write32(ps2vita::Memory::kVu1MicroBase +
                   static_cast<std::uint32_t>(index * 8u + 4u), 0x000002FFu);
  }
  memory.write32(ps2vita::Memory::kVu1MicroBase + 0x648u, 0x81E8137Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 0x64Cu, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1DataBase + 4u * 16u, 0xDEADBEEFu);

  ps2vita::Vu1 vu(memory);
  vu.start(0u);
  vu.run(6u);
  check(vu.state().vi[1] == 0u && vu.state().vi[2] == 5u &&
        vu.state().vi[3] == 0x16u && vu.state().vi[15] == 5u,
        "VU1 executes captured IADDIU/BAL prologue and its delay slot");
  check(vu.state().vf[8][0] == 0xDEADBEEFu && vu.state().pc == 0x650u,
        "VU1 BAL reaches the captured routine and LQI loads masked data");
}

void test_vu1_captured_matrix_pair() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x01E821BCu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x01E828BDu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 16u, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 20u, 0x01E830BEu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 24u, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 28u, 0x01E83B0Bu);

  ps2vita::Vu1 vu(memory);
  auto& state = vu.state();
  state.vf[4] = {{0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u}};
  state.vf[5] = state.vf[4];
  state.vf[6] = state.vf[4];
  state.vf[7] = state.vf[4];
  state.vf[8] = {{0x40000000u, 0x40400000u, 0x40800000u, 0x3F800000u}};
  vu.start(0u);
  vu.run(4u);
  check(vu.pairs_executed() == 4u &&
        state.vf[12][0] == 0x41200000u && // 1*2 + 1*3 + 1*4 + 1*1
        state.vf[12][3] == 0x42200000u,   // 4*2 + 4*3 + 4*4 + 4*1
        "VU1 executes the captured MULAx/MADDAy/MADDAz/MADDw dot product");
}

void test_vu1_sqi() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x81E3637Du);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  ps2vita::Vu1 vu(memory);
  vu.state().vi[3] = 7u;
  vu.state().vf[12] = {{1u, 2u, 3u, 4u}};
  vu.start(0u);
  vu.run(1u);
  check(vu.state().vi[3] == 8u &&
        memory.read32(ps2vita::Memory::kVu1DataBase + 7u * 16u) == 1u &&
        memory.read32(ps2vita::Memory::kVu1DataBase + 7u * 16u + 12u) == 4u,
        "VU1 SQI stores selected lanes and increments its address register");
}

void test_vu1_xgkick_packet() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x800016FCu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  const auto packet_address = ps2vita::Memory::kVu1DataBase + 4u * 16u;
  memory.write64(packet_address, (1ull << 60) | (1ull << 15) | 1ull);
  memory.write64(packet_address + 8u, 0xEull);
  memory.write64(packet_address + 16u, 0x0123456789ABCDEFull);
  memory.write64(packet_address + 24u, 0xFEDCBA9876543210ull);

  ps2vita::Vu1 vu(memory);
  vu.state().vi[2] = 4u;
  vu.start(0u);
  vu.run(1u);
  std::vector<std::uint8_t> packet;
  std::uint64_t payload = 0;
  check(vu.pop_path1_packet(packet) && packet.size() == 32u,
        "VU1 XGKICK snapshots one complete EOP GIF packet");
  if (packet.size() == 32u) std::memcpy(&payload, packet.data() + 16u, 8u);
  check(payload == 0x0123456789ABCDEFull,
        "VU1 XGKICK preserves path-1 GIF payload bytes");
}

void test_vu1_end_and_resume() {
  ps2vita::Memory memory;
  for (unsigned pair = 0; pair < 3u; ++pair) {
    memory.write32(ps2vita::Memory::kVu1MicroBase + pair * 8u, 0x8000033Cu);
    memory.write32(ps2vita::Memory::kVu1MicroBase + pair * 8u + 4u,
                   pair == 0u ? 0x400002FFu : 0x000002FFu);
  }
  ps2vita::Vu1 vu(memory);
  vu.start(0u);
  vu.run(10u);
  check(!vu.running() && vu.pairs_executed() == 2u && vu.state().pc == 0x10u,
        "VU1 E bit stops after exactly one delay pair");
  vu.resume();
  vu.run(1u);
  check(vu.running() && vu.pairs_executed() == 3u && vu.state().pc == 0x18u,
        "VU1 MSCNT-style resume continues at the retained TPC");
}

void test_vu1_mtir_xtop() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8001FBFCu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x800106BCu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x000002FFu);
  ps2vita::Vu1 vu(memory);
  vu.state().vf[31][0] = 0x12345678u;
  vu.set_top(0x155u);
  vu.start(0u);
  vu.run(2u);
  check(vu.state().vi[1] == 0x155u && vu.pairs_executed() == 2u,
        "VU1 MTIR and XTOP feed the captured continuation integer register");
}

void test_vu1_integer_branch_and_load() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x52010001u); // ibne vi1,vi0,+1
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 16u, 0x810A0BFEu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 20u, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1DataBase + 3u * 16u, 0x1234ABCDu);
  ps2vita::Vu1 vu(memory);
  vu.state().vi[1] = 3u;
  vu.start(0u);
  vu.run(3u);
  check(vu.state().pc == 0x18u && vu.state().vi[10] == 0xABCDu,
        "VU1 IBNE executes its delay pair then reaches masked ILWR");
}

void test_vu1_lq_sq() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x01EF0800u);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x03E27800u);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x000002FFu);
  for (unsigned lane = 0; lane < 4u; ++lane)
    memory.write32(ps2vita::Memory::kVu1DataBase + 5u * 16u + lane * 4u,
                   0xA0u + lane);
  ps2vita::Vu1 vu(memory);
  vu.state().vi[1] = 5u;
  vu.state().vi[2] = 9u;
  vu.start(0u);
  vu.run(2u);
  check(memory.read32(ps2vita::Memory::kVu1DataBase + 9u * 16u) == 0xA0u &&
        memory.read32(ps2vita::Memory::kVu1DataBase + 9u * 16u + 12u) == 0xA3u,
        "VU1 captured LQ/SQ pair copies all selected lanes between qwords");
}

void test_vu1_div_mulq() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x81F803BCu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x800003BFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x01C0C61Cu);
  ps2vita::Vu1 vu(memory);
  vu.state().vf[24] = {{0x40800000u, 0x40800000u, 0x40800000u, 0x40000000u}};
  vu.start(0u);
  vu.run(2u);
  check(vu.state().q == 0x3F000000u &&
        vu.state().vf[24][0] == 0x40000000u &&
        vu.state().vf[24][2] == 0x40000000u &&
        vu.state().vf[24][3] == 0x40000000u,
        "VU1 DIV/WAITQ/MULq normalizes captured XYZ lanes and preserves W");
}

void test_vu1_captured_max_sub() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x01E06B50u);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x01B897ACu);
  ps2vita::Vu1 vu(memory);
  vu.state().vf[13] = {{0xBF800000u, 0xC0000000u, 0xC0400000u, 0xC0800000u}};
  vu.state().vf[18] = {{0x40A00000u, 0x40A00000u, 0x40A00000u, 0x40A00000u}};
  vu.state().vf[24] = {{0x3F800000u, 0x40000000u, 0x40400000u, 0x40800000u}};
  vu.start(0u);
  vu.run(2u);
  check(vu.state().vf[13][0] == 0u && vu.state().vf[13][3] == 0u &&
        vu.state().vf[30][0] == 0x40800000u &&
        vu.state().vf[30][1] == 0x40400000u &&
        vu.state().vf[30][2] == 0u &&
        vu.state().vf[30][3] == 0x3F800000u,
        "VU1 captured MAXx/SUB sequence applies scalar, vector, and lane masks");
}

void test_vu1_ftoi4() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x01D3C17Du);
  ps2vita::Vu1 vu(memory);
  vu.state().vf[24] = {{0x3FC00000u, 0xBFC00000u, 0x3F000000u, 0x41200000u}};
  vu.state().vf[19][3] = 0xDEADBEEFu;
  vu.start(0u);
  vu.run(1u);
  check(vu.state().vf[19][0] == 24u &&
        vu.state().vf[19][1] == static_cast<std::uint32_t>(-24) &&
        vu.state().vf[19][2] == 8u && vu.state().vf[19][3] == 0xDEADBEEFu,
        "VU1 captured FTOI4 converts and masks scaled integer lanes");
}

void test_vu1_captured_ftoi0() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x01F9D17Cu);
  ps2vita::Vu1 vu(memory);
  vu.state().vf[26] = {{0x40200000u, 0xC0200000u, 0x7F800000u, 0x3F000000u}};
  vu.start(0u);
  vu.run(1u);
  check(vu.state().vf[25][0] == 2u &&
        vu.state().vf[25][1] == static_cast<std::uint32_t>(-2) &&
        vu.state().vf[25][2] == 0x7FFFFFFFu,
        "VU1 captured FTOI0 truncates normal lanes and saturates overflow");
}

void test_vu1_captured_iaddi() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x800A57F2u);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x000002FFu);
  ps2vita::Vu1 vu(memory);
  vu.state().vi[10] = 3u;
  vu.start(0u);
  vu.run(1u);
  check(vu.state().vi[10] == 2u,
        "VU1 captured IADDI sign-extends its five-bit negative immediate");
}

void test_vu1_fmand_prior_pair_flags() {
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x34016000u);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x01ED49BCu);
  ps2vita::Vu1 vu(memory);
  vu.state().mac = 0x00F0u;
  vu.state().vi[12] = 0x0050u;
  vu.state().vf[9][0] = 0x40000000u;
  vu.state().vf[13][0] = 0x40400000u;
  vu.start(0u);
  vu.run(1u);
  check(vu.state().vi[1] == 0x0050u,
        "VU1 FMAND reads the prior MAC flags from its paired upper instruction");
}

void test_vif1_top_relative_unpack() {
  std::array<std::uint32_t, 8> words{{
      0x03000020u, 0x02000010u, 0x14000000u, 0x6C018000u,
      1u, 2u, 3u, 4u,
  }};
  ps2vita::Memory memory;
  memory.write32(ps2vita::Memory::kVu1MicroBase, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 4u, 0x400002FFu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 8u, 0x8000033Cu);
  memory.write32(ps2vita::Memory::kVu1MicroBase + 12u, 0x000002FFu);
  ps2vita::Vif1 vif(memory);
  check(vif.submit(reinterpret_cast<const std::uint8_t*>(words.data()),
                   sizeof(words)) && vif.top() == 0x20u,
        "VIF1 MSCAL exposes the current double-buffer TOP to VU1");
  check(memory.read32(ps2vita::Memory::kVu1DataBase + 0x30u * 16u) == 1u &&
        memory.read32(ps2vita::Memory::kVu1DataBase + 0x30u * 16u + 12u) == 4u,
        "VIF1 top-relative UNPACK targets the post-MSCAL TOPS buffer");
}

void test_captured_bios_gif_sprite() {
  // First packet emitted by the retail BIOS after CDVD/SIF initialization.
  constexpr std::array<std::array<std::uint64_t, 2>, 15> qwords{{
      {{0x100000000000800Eull, 0x000000000000000Eull}},
      {{0x00000000000A0050ull, 0x000000000000004Cull}},
      {{0x00000000000000A0ull, 0x000000000000004Eull}},
      {{0x0000780000006C00ull, 0x0000000000000018ull}},
      {{0x00FF0000027F0000ull, 0x0000000000000040ull}},
      {{0x0000000000000001ull, 0x000000000000001Aull}},
      {{0x0000000000000001ull, 0x0000000000000046ull}},
      {{0x0000000000000000ull, 0x0000000000000045ull}},
      {{0x0000000000050000ull, 0x0000000000000047ull}},
      {{0x0000000000030000ull, 0x0000000000000047ull}},
      {{0x0000000000000006ull, 0x0000000000000000ull}},
      {{0x3F80000000000000ull, 0x0000000000000001ull}},
      {{0x0000000078006C00ull, 0x0000000000000005ull}},
      {{0x0000000088009400ull, 0x0000000000000005ull}},
      {{0x0000000000050000ull, 0x0000000000000047ull}},
  }};
  std::vector<std::uint8_t> packet(sizeof(qwords));
  std::memcpy(packet.data(), qwords.data(), packet.size());

  ps2vita::Gs gs;
  gs.clear(0xFFFF00FFu);
  ps2vita::Gif gif(gs);
  check(gif.submit(packet.data(), packet.size()),
        "GIF frontend accepts the captured packed A+D packet");
  check(gs.pixel(0, 0) == 0u && gs.pixel(159, 63) == 0u &&
        gs.pixel(80, 64) == 0xFFFF00FFu,
        "captured BIOS sprite clears exactly the quarter-scale 640x256 region");
}

void test_gif_reglist_sprite() {
  // One REGLIST tag: PRIM, RGBAQ, XYZ2, XYZ2. Four 64-bit values consume two
  // qwords and exercise the format's different NLOOP accounting.
  constexpr std::array<std::uint64_t, 6> words{{
      0x4400000000008001ull, 0x0000000000005510ull,
      0x0000000000000006ull, 0xFF332211ull,
      0x0000000000100010ull, 0x0000000001100110ull,
  }};
  std::vector<std::uint8_t> packet(sizeof(words));
  std::memcpy(packet.data(), words.data(), packet.size());
  ps2vita::Gs gs;
  gs.clear(0u);
  ps2vita::Gif gif(gs);
  check(gif.submit(packet.data(), packet.size()) &&
        gif.reglist_tags() == 1u && gif.packets_rejected() == 0u,
        "GIF frontend consumes a complete REGLIST tag");
  check(gs.pixel(0, 0) == 0xFF332211u && gs.pixel(3, 3) == 0xFF332211u &&
        gs.pixel(4, 4) == 0u,
        "GIF REGLIST registers emit a masked quarter-scale sprite");
}

void test_gif_image_continues_to_pre_primitive() {
  // One IMAGE qword followed by a PRE=1 packed point tag. Raw image data has
  // no descriptors and must not terminate parsing of the enclosing DMA packet.
  constexpr std::array<std::array<std::uint64_t, 2>, 4> qwords{{
      {{0x0800000000008001ull, 0u}},
      {{0x0123456789ABCDEFull, 0xFEDCBA9876543210ull}},
      {{0x1000400000008001ull, 0x0000000000000005ull}},
      {{0x0000000000100010ull, 0u}},
  }};
  std::vector<std::uint8_t> packet(sizeof(qwords));
  std::memcpy(packet.data(), qwords.data(), packet.size());
  ps2vita::Gs gs;
  gs.clear(0u);
  ps2vita::Gif gif(gs);
  check(gif.submit(packet.data(), 24u) && gif.image_tags() == 0u &&
        gif.submit(packet.data() + 24u, packet.size() - 24u) &&
        gif.image_tags() == 1u && gif.image_bytes() == 16u &&
        gif.packets_rejected() == 0u,
        "GIF IMAGE traversal spans DMA bursts and reaches a following tag");
  check(gs.pixel(0, 0) == 0x80808080u,
        "GIF PRE field selects the primitive before packed XYZ2");
}

void test_gif_psmct32_host_to_local_transfer() {
  constexpr std::array<std::array<std::uint64_t, 2>, 7> qwords{{
      {{0x1000000000008004ull, 0xEull}},
      {{0x0001000100000000ull, 0x50ull}}, // DBP=1, DBW=1, PSMCT32
      {{0u, 0x51ull}},
      {{0x0000000200000002ull, 0x52ull}}, // 2x2 rectangle
      {{0u, 0x53ull}},
      {{0x0800000000008001ull, 0u}},
      {{0x2222222211111111ull, 0x4444444433333333ull}},
  }};
  std::vector<std::uint8_t> packet(sizeof(qwords));
  std::memcpy(packet.data(), qwords.data(), packet.size());
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  check(gif.submit(packet.data(), packet.size()) &&
        gif.local_bytes_written() == 16u,
        "GIF PSMCT32 IMAGE writes a host-to-local rectangle");
  check(gif.read_local32(0x100u) == 0x11111111u &&
        gif.read_local32(0x104u) == 0x22222222u &&
        gif.read_local32(0x200u) == 0x33333333u &&
        gif.read_local32(0x204u) == 0x44444444u,
        "GS local-memory oracle applies DBP, DBW, and row stride");
}

void test_gif_textured_sprite_from_local_memory() {
  constexpr std::array<std::array<std::uint64_t, 2>, 7> upload{{
      {{0x1000000000008004ull, 0xEull}},
      {{0x0001000100000000ull, 0x50ull}},
      {{0u, 0x51ull}},
      {{0x0000000200000002ull, 0x52ull}},
      {{0u, 0x53ull}},
      {{0x0800000000008001ull, 0u}},
      {{0xFF00FF00FF0000FFull, 0xFFFFFFFFFFFF0000ull}},
  }};
  constexpr std::uint64_t tex0 = 1ull | (1ull << 14) | (1ull << 26) |
      (1ull << 30) | (1ull << 34) | (1ull << 35);
  constexpr std::array<std::array<std::uint64_t, 2>, 8> draw{{
      {{0x1000000000008002ull, 0xEull}},
      {{tex0, 0x06ull}},
      {{0x116ull, 0x00ull}}, // SPRITE + TME + FST
      {{0x2000000000008002ull, 0x53ull}},
      {{0u, 0u}},
      {{0u, 0u}},
      {{0x00200020ull, 0u}},
      {{0x00800080ull, 0u}},
  }};
  ps2vita::Gs gs;
  gs.clear(0u);
  ps2vita::Gif gif(gs);
  check(gif.submit(reinterpret_cast<const std::uint8_t*>(upload.data()),
                   sizeof(upload)) &&
        gif.submit(reinterpret_cast<const std::uint8_t*>(draw.data()),
                   sizeof(draw)),
        "GIF textured-sprite fixture uploads and draws");
  check(gs.pixel(0, 0) == 0xFF0000FFu &&
        gs.pixel(1, 0) == 0xFF00FF00u &&
        gs.pixel(0, 1) == 0xFFFF0000u &&
        gs.pixel(1, 1) == 0xFFFFFFFFu,
        "GIF UV sprite samples the logical PSMCT32 surface");
}

void put16(std::vector<std::uint8_t>& v, std::size_t at, std::uint16_t x) {
  v[at] = static_cast<std::uint8_t>(x); v[at + 1] = static_cast<std::uint8_t>(x >> 8);
}
void put32(std::vector<std::uint8_t>& v, std::size_t at, std::uint32_t x) {
  put16(v, at, static_cast<std::uint16_t>(x)); put16(v, at + 2, static_cast<std::uint16_t>(x >> 16));
}

std::vector<std::uint8_t> tiny_elf() {
  std::vector<std::uint8_t> v(0x108, 0);
  v[0] = 0x7F; v[1] = 'E'; v[2] = 'L'; v[3] = 'F'; v[4] = 1; v[5] = 1; v[6] = 1;
  put16(v, 16, 2); put16(v, 18, 8); put32(v, 20, 1);
  put32(v, 24, 0x1000); put32(v, 28, 52);
  put16(v, 40, 52); put16(v, 42, 32); put16(v, 44, 1);
  put32(v, 52, 1); put32(v, 56, 0x100); put32(v, 60, 0x1000);
  put32(v, 68, 8); put32(v, 72, 16); put32(v, 76, 5); put32(v, 80, 16);
  put32(v, 0x100, i_type(0x09, 0, 2, 42));
  put32(v, 0x104, 0x0000000D);
  return v;
}

void test_elf_and_emulator() {
  auto elf = tiny_elf();
  ps2vita::Emulator emulator;
  const auto loaded = emulator.load_elf(elf.data(), elf.size());
  check(loaded.ok && loaded.entry == 0x1000 && loaded.segments == 1, "ELF load metadata");
  check(emulator.memory().read32(0x1008) == 0, "ELF BSS zero-fill");
  check(emulator.run_slice(10) == ps2vita::StopReason::Break, "loaded ELF executes");
  check(emulator.cpu().state().gpr[2] == 42, "ELF program result");

  elf[18] = 3;
  check(!emulator.load_elf(elf.data(), elf.size()).ok, "reject non-MIPS ELF");
}

void test_phase0_aot_contract() {
  const auto& package = ps2vita::phase0_aot_package();
  check(package.name != nullptr && package.entry_count >= 22u,
        "Phase-0 AOT package exposes sorted entry metadata");
  check(ps2vita::validate_aot_package(package).ok(),
        "Phase-0 AOT package passes metadata validation");
  const auto* entry = ps2vita::find_phase0_aot_function(0x1000u);
  check(entry != nullptr && entry->guest_start == 0x1000u &&
            entry->guest_end == 0x100Cu && entry->function != nullptr,
        "Phase-0 AOT function table resolves its entry");
  check(ps2vita::find_phase0_aot_function(0x0FFCu) == nullptr &&
            ps2vita::find_phase0_aot_function(0x1004u) == nullptr &&
            ps2vita::find_phase0_aot_function(0x100Cu) == nullptr,
        "Phase-0 AOT table accepts only generated entry points");
  check(ps2vita::find_aot_function(package, 0x3008u) != nullptr &&
            ps2vita::find_aot_function(package, 0x3010u) == nullptr,
        "generic AOT package binary lookup resolves resume entries only");

  ps2vita::AotFunctionEntry invalid_entries[] = {
      package.entries[1], package.entries[0]};
  auto invalid_package = package;
  invalid_package.entries = invalid_entries;
  invalid_package.entry_count = 2u;
  check(ps2vita::validate_aot_package(invalid_package).error ==
            ps2vita::AotPackageError::UnsortedEntries,
        "AOT validation rejects unsorted generated entries");
  ps2vita::Memory invalid_memory;
  ps2vita::CpuState invalid_state{};
  invalid_state.pc = invalid_entries[0].guest_start;
  check(ps2vita::execute_aot(invalid_package, invalid_memory, invalid_state).kind ==
            ps2vita::AotExitKind::Interpreter,
        "AOT execution safely rejects an invalid package");

  invalid_entries[0] = package.entries[0];
  invalid_entries[1] = package.entries[1];
  invalid_entries[1].guest_start = invalid_entries[0].guest_start + 4u;
  check(ps2vita::validate_aot_package(invalid_package).error ==
            ps2vita::AotPackageError::OverlappingEntries,
        "AOT validation rejects overlapping generated ranges");

  invalid_package = package;
  invalid_package.abi_version = ps2vita::kAotAbiVersion + 1u;
  check(ps2vita::validate_aot_package(invalid_package).error ==
            ps2vita::AotPackageError::UnsupportedAbi,
        "AOT validation rejects an incompatible runtime ABI");

  invalid_package = package;
  invalid_package.source_fingerprint_sha256 = "not-a-sha256";
  check(ps2vita::validate_aot_package(invalid_package).error ==
            ps2vita::AotPackageError::InvalidFingerprint,
        "AOT validation rejects a malformed source fingerprint");

  ps2vita::Memory fallback_memory;
  ps2vita::CpuState fallback_state{};
  fallback_state.pc = 0x5000u;
  const auto fallback =
      ps2vita::execute_phase0_aot(fallback_memory, fallback_state);
  check(fallback.kind == ps2vita::AotExitKind::Interpreter &&
            fallback.target == 0x5000u && fallback.instructions == 0u,
        "unknown AOT entry returns an interpreter exit");

  ps2vita::Memory bounded_memory;
  ps2vita::CpuState bounded_state{};
  bounded_state.pc = 0x3000u;
  const auto bounded =
      ps2vita::dispatch_phase0_aot(bounded_memory, bounded_state, 1u);
#if defined(PS2VITA_ASTRART_DIRECT_TRACES)
  check(bounded.kind == ps2vita::AotExitKind::Interpreter &&
            bounded.target == 0x3008u && bounded.instructions == 5u &&
            bounded_state.aot_trace_entries == 1u &&
            bounded_state.aot_trace_horizon_fallbacks == 0u,
        "direct trace batches proven blocks within one dispatch budget");
  ps2vita::Memory boundary_memory;
  boundary_memory.advance(3u);
  ps2vita::CpuState boundary_state{};
  boundary_state.pc = 0x3000u;
  const auto boundary =
      ps2vita::dispatch_phase0_aot(boundary_memory, boundary_state, 1u);
  check(boundary.kind == ps2vita::AotExitKind::Interpreter &&
            boundary.target == 0x3020u && boundary.instructions == 2u &&
            boundary_state.aot_trace_entries == 1u &&
            boundary_state.aot_trace_horizon_fallbacks == 1u,
        "direct trace falls back before crossing the event horizon");
#else
  check(bounded.kind == ps2vita::AotExitKind::Interpreter &&
            bounded.target == 0x3020u && bounded.instructions == 2u,
        "AOT dispatch budget yields safely to interpreter");
#endif

  const auto result = ps2vita::run_phase0_aot_probe();
  check(result.matched, "Phase-0 interpreter/AOT contract matches");
  check(result.interpreter_stop == ps2vita::StopReason::Break &&
            result.aot_stop == ps2vita::StopReason::Break,
        "Phase-0 paths observe BREAK");
  check(result.interpreter_value == 42u && result.aot_value == 42u,
        "Phase-0 paths produce identical guest RAM");

  const auto chain = ps2vita::run_phase0_aot_chain_probe();
  check(chain.matched, "Phase-0 direct/indirect AOT chain matches interpreter");
  check(chain.interpreter_stop == ps2vita::StopReason::Break &&
            chain.aot_stop == ps2vita::StopReason::Break &&
            chain.interpreter_value == 42u && chain.aot_value == 42u,
        "Phase-0 chained paths return and store identical state");

  const auto benchmark = ps2vita::run_phase0_aot_benchmark(nullptr, 16u, 1u);
  check(benchmark.matched,
        "fully translated performance workload matches interpreter state");
  check(benchmark.interpreter_stop == ps2vita::StopReason::Break &&
            benchmark.aot_stop == ps2vita::StopReason::Break &&
            benchmark.guest_instructions > 1000u &&
            benchmark.interpreter_checksum == benchmark.aot_checksum &&
            benchmark.trace_probe_guest_instructions == 128u &&
            benchmark.trace_probe_checksum != 0u,
        "performance workload covers a substantial deterministic guest trace");
#if defined(PS2VITA_ASTRART_DIRECT_TRACES)
  check(benchmark.trace_entries == 16u,
        "trace benchmark records one fused entry per chain iteration");
#else
  check(benchmark.trace_entries == 0u,
        "legacy benchmark reports no fused trace entries");
#endif

  // Generated load/store and signed-arithmetic semantics are compared over a
  // deterministic spread of inputs rather than a single friendly value.
  std::uint32_t seed = 0xC001D00Du;
  bool fuzz_matched = true;
  for (unsigned iteration = 0; iteration < 32u; ++iteration) {
    seed = seed * 1664525u + 1013904223u;
    const std::uint32_t a0 = seed;
    seed = seed * 1664525u + 1013904223u;
    const std::uint32_t a1 = seed;

    ps2vita::Memory interpreter_memory;
    interpreter_memory.write32(0x4000u, 0x00851021u);
    interpreter_memory.write32(0x4004u, 0xACC20000u);
    interpreter_memory.write32(0x4008u, 0x8CC30000u);
    interpreter_memory.write32(0x400Cu, 0x2462FFF9u);
    interpreter_memory.write32(0x4010u, 0x0000000Du);
    ps2vita::Cpu interpreter(interpreter_memory);
    interpreter.reset(0x4000u);
    interpreter.state().gpr[4] = a0;
    interpreter.state().gpr[5] = a1;
    interpreter.state().gpr[6] = 0x2100u;
    const auto interpreter_stop = interpreter.run(8u);

    ps2vita::Memory generated_memory;
    ps2vita::CpuState generated_state{};
    generated_state.pc = 0x4000u;
    generated_state.gpr[4] = a0;
    generated_state.gpr[5] = a1;
    generated_state.gpr[6] = 0x2100u;
    const auto generated_exit =
        ps2vita::dispatch_aot(package, generated_memory, generated_state, 2u);
    fuzz_matched = fuzz_matched &&
        interpreter_stop == generated_exit.reason &&
        generated_exit.kind == ps2vita::AotExitKind::Stop &&
        interpreter.state().gpr[2] == generated_state.gpr[2] &&
        interpreter.state().gpr[3] == generated_state.gpr[3] &&
        interpreter_memory.read32(0x2100u) ==
            generated_memory.read32(0x2100u) &&
        interpreter.state().pc == generated_state.pc &&
        interpreter.state().cycles == generated_state.cycles;
  }
  check(fuzz_matched,
        "generated arithmetic/load/store blocks match seeded interpreter cases");

  ps2vita::Memory fault_memory;
  fault_memory.write32(0x4000u, 0x00851021u);
  fault_memory.write32(0x4004u, 0xACC20000u);
  fault_memory.write32(0x4008u, 0x8CC30000u);
  fault_memory.write32(0x400Cu, 0x2462FFF9u);
  fault_memory.write32(0x4010u, 0x0000000Du);
  ps2vita::Cpu fault_interpreter(fault_memory);
  fault_interpreter.reset(0x4000u);
  fault_interpreter.state().gpr[4] = 10u;
  fault_interpreter.state().gpr[5] = 20u;
  fault_interpreter.state().gpr[6] = 0x2101u;
  const auto fault_stop = fault_interpreter.run(8u);

  ps2vita::Memory fault_generated_memory;
  ps2vita::CpuState fault_generated_state{};
  fault_generated_state.pc = 0x4000u;
  fault_generated_state.gpr[4] = 10u;
  fault_generated_state.gpr[5] = 20u;
  fault_generated_state.gpr[6] = 0x2101u;
  const auto fault_exit = ps2vita::execute_aot(
      package, fault_generated_memory, fault_generated_state);
  check(fault_exit.kind == ps2vita::AotExitKind::Interpreter &&
            fault_exit.target == 0x4004u && fault_exit.instructions == 1u &&
            fault_generated_state.pc == 0x4004u &&
            fault_generated_state.cycles == 1u &&
            fault_stop == ps2vita::StopReason::MemoryFault,
        "generated misaligned store yields before the faulting instruction");

  ps2vita::Memory unsupported_memory;
  ps2vita::CpuState unsupported_state{};
  unsupported_state.pc = 0x5000u;
  const auto unsupported =
      ps2vita::execute_aot(package, unsupported_memory, unsupported_state);
  check(unsupported.kind == ps2vita::AotExitKind::Interpreter &&
            unsupported.target == 0x5000u && unsupported.instructions == 0u &&
            unsupported_state.pc == 0x5000u,
        "generated unsupported opcode exits exactly to the interpreter");

  bool hot_scalar_matched = true;
  for (unsigned iteration = 0; iteration < 32u; ++iteration) {
    seed = seed * 1664525u + 1013904223u;
    const std::uint64_t a0 = (static_cast<std::uint64_t>(seed) << 32) |
                             (seed ^ 0xA5A5A5A5u);
    seed = seed * 1664525u + 1013904223u;
    const std::uint64_t a1 = (static_cast<std::uint64_t>(seed) << 32) |
                             (seed ^ 0x5A5A5A5Au);

    ps2vita::Memory interpreter_memory;
    interpreter_memory.write32(0x8000u, 0x3C02ABCDu);
    interpreter_memory.write32(0x8004u, 0x34421234u);
    interpreter_memory.write32(0x8008u, 0x3083F0F0u);
    interpreter_memory.write32(0x800Cu, 0x00434024u);
    interpreter_memory.write32(0x8010u, 0x0085482Bu);
    interpreter_memory.write32(0x8014u, 0x0000000Du);
    ps2vita::Cpu interpreter(interpreter_memory);
    interpreter.reset(0x8000u);
    interpreter.state().gpr[4] = a0;
    interpreter.state().gpr[5] = a1;
    interpreter.state().gpr_hi[2] = 0x1111222233334444ull;
    interpreter.state().gpr_hi[3] = 0x5555666677778888ull;
    interpreter.state().gpr_hi[8] = 0x9999AAAABBBBCCCCull;
    interpreter.state().gpr_hi[9] = 0xDDDDEEEEFFFF0000ull;
    const auto interpreter_stop = interpreter.run(8u);

    ps2vita::Memory generated_memory;
    ps2vita::CpuState generated_state{};
    generated_state.pc = 0x8000u;
    generated_state.gpr[4] = a0;
    generated_state.gpr[5] = a1;
    generated_state.gpr_hi[2] = 0x1111222233334444ull;
    generated_state.gpr_hi[3] = 0x5555666677778888ull;
    generated_state.gpr_hi[8] = 0x9999AAAABBBBCCCCull;
    generated_state.gpr_hi[9] = 0xDDDDEEEEFFFF0000ull;
    const auto generated_exit = ps2vita::dispatch_aot(
        package, generated_memory, generated_state, 2u);
    hot_scalar_matched = hot_scalar_matched &&
        interpreter_stop == generated_exit.reason &&
        generated_exit.kind == ps2vita::AotExitKind::Stop &&
        generated_exit.instructions == 6u &&
        interpreter.state().pc == generated_state.pc &&
        interpreter.state().cycles == generated_state.cycles;
    for (const unsigned reg : {2u, 3u, 8u, 9u}) {
      hot_scalar_matched = hot_scalar_matched &&
          interpreter.state().gpr[reg] == generated_state.gpr[reg] &&
          interpreter.state().gpr_hi[reg] == generated_state.gpr_hi[reg];
    }
  }
  check(hot_scalar_matched,
        "profile-guided scalar AOT families match seeded interpreter state");

  bool branch_matched = true;
  for (unsigned iteration = 0; iteration < 16u; ++iteration) {
    const std::uint32_t a0 = iteration * 0x1020304u;
    const std::uint32_t a1 = (iteration & 1u) ? a0 : a0 + 1u;
    ps2vita::Memory interpreter_memory;
    interpreter_memory.write32(0x6000u, 0x10850003u);
    interpreter_memory.write32(0x6004u, 0x24020001u);
    interpreter_memory.write32(0x6008u, 0x08001806u);
    interpreter_memory.write32(0x600Cu, 0x24420002u);
    interpreter_memory.write32(0x6010u, 0x08001806u);
    interpreter_memory.write32(0x6014u, 0x24420004u);
    interpreter_memory.write32(0x6018u, 0x0000000Du);
    ps2vita::Cpu interpreter(interpreter_memory);
    interpreter.reset(0x6000u);
    interpreter.state().gpr[4] = a0;
    interpreter.state().gpr[5] = a1;
    const auto interpreter_stop = interpreter.run(8u);

    ps2vita::Memory generated_memory;
    ps2vita::CpuState generated_state{};
    generated_state.pc = 0x6000u;
    generated_state.gpr[4] = a0;
    generated_state.gpr[5] = a1;
    const auto generated_exit =
        ps2vita::dispatch_aot(package, generated_memory, generated_state, 4u);
    branch_matched = branch_matched &&
        interpreter_stop == generated_exit.reason &&
        generated_exit.kind == ps2vita::AotExitKind::Stop &&
        interpreter.state().gpr[2] == generated_state.gpr[2] &&
        interpreter.state().pc == generated_state.pc &&
        interpreter.state().cycles == generated_state.cycles &&
        generated_exit.instructions == 5u;
  }
  check(branch_matched,
        "generated direct branches and delay slots match both interpreter paths");

  bool bne_matched = true;
  for (unsigned iteration = 0; iteration < 8u; ++iteration) {
    const std::uint32_t a0 = iteration * 0x112233u;
    const std::uint32_t a1 = (iteration & 1u) ? a0 + 1u : a0;
    ps2vita::Memory interpreter_memory;
    interpreter_memory.write32(0x7000u, 0x14850003u);
    interpreter_memory.write32(0x7004u, 0x24020001u);
    interpreter_memory.write32(0x7008u, 0x08001C06u);
    interpreter_memory.write32(0x700Cu, 0x24420002u);
    interpreter_memory.write32(0x7010u, 0x08001C06u);
    interpreter_memory.write32(0x7014u, 0x24420004u);
    interpreter_memory.write32(0x7018u, 0x0000000Du);
    ps2vita::Cpu interpreter(interpreter_memory);
    interpreter.reset(0x7000u);
    interpreter.state().gpr[4] = a0;
    interpreter.state().gpr[5] = a1;
    const auto interpreter_stop = interpreter.run(8u);

    ps2vita::Memory generated_memory;
    ps2vita::CpuState generated_state{};
    generated_state.pc = 0x7000u;
    generated_state.gpr[4] = a0;
    generated_state.gpr[5] = a1;
    const auto generated_exit =
        ps2vita::dispatch_aot(package, generated_memory, generated_state, 4u);
    bne_matched = bne_matched &&
        interpreter_stop == generated_exit.reason &&
        generated_exit.kind == ps2vita::AotExitKind::Stop &&
        interpreter.state().gpr[2] == generated_state.gpr[2] &&
        interpreter.state().pc == generated_state.pc &&
        interpreter.state().cycles == generated_state.cycles;
  }
  check(bne_matched,
        "generated BNE blocks match taken and fallthrough interpreter paths");
}
}

int main() {
  test_execution_census_blocks_and_edges();
  test_memory_aliases();
  test_bios_mapping_and_boot();
  test_iop_memory_and_cpu();
  test_sif1_dma_and_external_interrupts();
  test_sif1_continuation_chain();
  test_sif1_zero_length_next_chain();
  test_sif0_dma_reply();
  test_sif0_iop_side_completes_first();
  test_iop_timer5_deadlines();
  test_event_horizon_contract();
  test_cdvd_reset_status();
  test_sio2_disconnected_transfer();
  test_video_vblank_deadlines();
  test_spu2_dma4_completion();
  test_spu2_dma7_completion();
  test_ee_timer3_hblank_clock();
  test_exception_entry_and_eret();
  test_cop0_count_advances();
  test_di_ei_status();
  test_empty_ram_vector_falls_back_to_rom();
  test_tlb_translation();
  test_ee_internal_registers();
  test_absent_dev_board_window();
  test_cpu_delay_slot();
  test_interrupt_waits_for_branch_delay_slot();
  test_unaligned_memory_ops();
  test_quadword_load_store();
  test_compiler_support_ops();
  test_doubleword_arithmetic();
  test_decoded_block_cache();
  test_mmi_padduw();
  test_mmi_por();
  test_mmi_plzcw();
  test_native_zero_fill_fast_path();
  test_mmi_div1_accumulator();
  test_mmi_packed_accumulator_moves();
  test_mmi_pcpyld();
  test_mmi_pextlw();
  test_r5900_shift_amount_moves();
  test_r5900_three_operand_multiply();
  test_scalar_fpu();
  test_fpu_memory_transfer();
  test_vu_memory_windows();
  test_vu0_cop2_transfers();
  test_quarter_scale_gs();
  test_gif_normal_dma_completion();
  test_vif1_source_chain_completion();
  test_vif1_mpg_upload();
  test_vif1_v4_32_unpack();
  test_vu1_captured_prologue();
  test_vu1_captured_matrix_pair();
  test_vu1_sqi();
  test_vu1_xgkick_packet();
  test_vu1_end_and_resume();
  test_vu1_mtir_xtop();
  test_vu1_integer_branch_and_load();
  test_vu1_lq_sq();
  test_vu1_div_mulq();
  test_vu1_captured_max_sub();
  test_vu1_ftoi4();
  test_vu1_captured_ftoi0();
  test_vu1_captured_iaddi();
  test_vu1_fmand_prior_pair_flags();
  test_vif1_top_relative_unpack();
  test_captured_bios_gif_sprite();
  test_gif_reglist_sprite();
  test_gif_image_continues_to_pre_primitive();
  test_gif_psmct32_host_to_local_transfer();
  test_gif_textured_sprite_from_local_memory();
  test_elf_and_emulator();
  test_phase0_aot_contract();
  if (failures) return EXIT_FAILURE;
  std::puts("All PS2Vita core tests passed.");
  return EXIT_SUCCESS;
}
