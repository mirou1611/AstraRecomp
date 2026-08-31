#include "ps2vita/emulator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TraceEntry {
  std::uint32_t pc = 0;
  std::uint32_t instruction = 0;
  std::uint64_t v0 = 0;
  std::uint64_t a0 = 0;
  std::uint64_t ra = 0;
};

struct IopTraceEntry {
  std::uint32_t pc = 0;
  std::uint32_t instruction = 0;
  std::uint32_t v0 = 0;
  std::uint32_t v1 = 0;
  std::uint32_t a0 = 0;
  std::uint32_t ra = 0;
};

struct CacheEntry {
  std::uint32_t pc = 0;
  std::uint32_t instruction = 0;
  std::uint32_t address = 0;
  std::uint32_t status = 0;
};

struct StoreEntry {
  std::uint32_t pc = 0;
  std::uint32_t instruction = 0;
  std::uint32_t address = 0;
  std::uint64_t value_lo = 0;
  std::uint64_t value_hi = 0;
};

struct Sif0Event {
  std::uint64_t step = 0;
  bool active = false;
  std::uint32_t tadr = 0;
  std::uint32_t source = 0;
  std::uint32_t words = 0;
  std::uint32_t ee_tag = 0;
  std::uint32_t destination = 0;
};

struct SyscallEvent {
  std::uint64_t step = 0;
  std::uint32_t pc = 0;
  std::uint64_t number = 0;
  std::uint64_t a0 = 0;
  std::uint64_t ra = 0;
  std::uint64_t sp = 0;
};

std::uint32_t parse_address(const char* text) {
  return static_cast<std::uint32_t>(std::strtoul(text, nullptr, 0));
}

// Group encodings at the same boundaries used by the interpreters. This keeps
// immediates and register choices from fragmenting the profile while retaining
// the SPECIAL, REGIMM, COP and MMI selectors that decide implementation work.
std::uint16_t opcode_family(std::uint32_t instruction) {
  const auto primary = static_cast<std::uint16_t>(instruction >> 26);
  std::uint16_t selector = 0;
  if (primary == 0u || primary == 0x1Cu)
    selector = static_cast<std::uint16_t>(instruction & 0x3Fu);
  else if (primary == 1u)
    selector = static_cast<std::uint16_t>((instruction >> 16) & 0x1Fu);
  else if (primary >= 0x10u && primary <= 0x12u) {
    const auto rs = static_cast<std::uint16_t>((instruction >> 21) & 0x1Fu);
    selector = rs == 0x10u
        ? static_cast<std::uint16_t>(0x20u | (instruction & 0x1Fu)) : rs;
  }
  return static_cast<std::uint16_t>((primary << 6) | selector);
}

std::string opcode_family_name(std::uint16_t family) {
  static constexpr const char* primary_names[64] = {
      "SPECIAL", "REGIMM", "J", "JAL", "BEQ", "BNE", "BLEZ", "BGTZ",
      "ADDI", "ADDIU", "SLTI", "SLTIU", "ANDI", "ORI", "XORI", "LUI",
      "COP0", "COP1", "COP2", "COP3", "BEQL", "BNEL", "BLEZL", "BGTZL",
      "DADDI", "DADDIU", "LDL", "LDR", "MMI", "UNASSIGNED_1D",
      "LQ", "SQ", "LB", "LH", "LWL", "LW", "LBU", "LHU", "LWR", "LWU",
      "SB", "SH", "SWL", "SW", "SDL", "SDR", "SWR", "CACHE",
      "LL", "LWC1", "LWC2", "PREF", "LLD", "LDC1", "LQC2", "LD",
      "SC", "SWC1", "SWC2", "UNASSIGNED_3B", "SCD", "SDC1", "SQC2", "SD"};
  const auto primary = static_cast<unsigned>(family >> 6);
  const auto selector = static_cast<unsigned>(family & 0x3Fu);
  if (primary == 0u) {
    static constexpr const char* special_names[64] = {
        "SLL/NOP", nullptr, "SRL", "SRA", "SLLV", nullptr, "SRLV", "SRAV",
        "JR", "JALR", "MOVZ", "MOVN", "SYSCALL", "BREAK", nullptr, "SYNC",
        "MFHI", "MTHI", "MFLO", "MTLO", "DSLLV", nullptr, "DSRLV", "DSRAV",
        "MULT", "MULTU", "DIV", "DIVU", nullptr, nullptr, nullptr, nullptr,
        "ADD", "ADDU", "SUB", "SUBU", "AND", "OR", "XOR", "NOR",
        "MFSA", "MTSA", "SLT", "SLTU", "DADD", "DADDU", "DSUB", "DSUBU",
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        "DSLL", nullptr, "DSRL", "DSRA", "DSLL32", nullptr, "DSRL32", "DSRA32"};
    if (special_names[selector]) return special_names[selector];
  }
  if (primary == 1u) {
    static constexpr const char* regimm_names[32] = {
        "BLTZ", "BGEZ", "BLTZL", "BGEZL", nullptr, nullptr, nullptr, nullptr,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr,
        "BLTZAL", "BGEZAL", "BLTZALL", "BGEZALL"};
    if (regimm_names[selector]) return regimm_names[selector];
  }
  if (primary == 0x1Cu && selector == 0x29u) return "PCPYUD";
  std::string name = primary_names[primary];
  if (primary == 0u || primary == 1u || primary == 0x1Cu ||
      (primary >= 0x10u && primary <= 0x12u)) {
    char suffix[8]{};
    std::snprintf(suffix, sizeof(suffix), "/%02X", selector);
    name += suffix;
  }
  return name;
}

void print_opcode_profile(const char* processor,
                          const std::array<std::uint64_t, 4096>& counts,
                          std::uint64_t total) {
  std::vector<std::pair<std::uint64_t, std::uint16_t>> ranked;
  for (std::uint16_t family = 0; family < counts.size(); ++family) {
    if (counts[family] != 0u) ranked.emplace_back(counts[family], family);
  }
  std::sort(ranked.begin(), ranked.end(), [](const auto& left,
                                              const auto& right) {
    return left.first != right.first ? left.first > right.first
                                     : left.second < right.second;
  });
  std::printf("%s dynamic opcode families (top 24 of %zu, total=%llu):\n",
      processor, ranked.size(), static_cast<unsigned long long>(total));
  const auto limit = std::min<std::size_t>(ranked.size(), 24u);
  for (std::size_t i = 0; i < limit; ++i) {
    const auto percent = total == 0u ? 0.0
        : 100.0 * static_cast<double>(ranked[i].first) /
              static_cast<double>(total);
    std::printf("%2zu %-16s %12llu %6.2f%%\n", i + 1,
        opcode_family_name(ranked[i].second).c_str(),
        static_cast<unsigned long long>(ranked[i].first), percent);
  }
}

} // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 10) {
    std::fprintf(stderr,
        "usage: ps2bios_trace BIOS [STOP_PC] [MAX_STEPS] [STOP_HIT] "
        "[WATCH_LOW_CLEAR] [IOP_DIVISOR] [IOP_STOP_PC] [SBUS_PROBE_STEP] "
        "[TIMER5_PROBE_STEP]\n");
    return 2;
  }

  std::ifstream input(argv[1], std::ios::binary);
  const std::vector<std::uint8_t> bios(
      (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (!input && bios.empty()) {
    std::fprintf(stderr, "could not read BIOS: %s\n", argv[1]);
    return 2;
  }

  const std::uint32_t stop_pc = argc >= 3 ? parse_address(argv[2]) : 0x9FC42170u;
  const std::uint64_t max_steps = argc >= 4
      ? std::strtoull(argv[3], nullptr, 0) : 100000000ull;
  const std::uint64_t stop_hit = argc >= 5
      ? std::strtoull(argv[4], nullptr, 0) : 1ull;
  const bool watch_low_clear = argc >= 6 && std::strtoul(argv[5], nullptr, 0);
  const std::uint64_t iop_divisor = argc >= 7
      ? std::strtoull(argv[6], nullptr, 0) : 8ull;
  if (iop_divisor == 0u) {
    std::fputs("IOP_DIVISOR must be non-zero\n", stderr);
    return 2;
  }
  const std::uint32_t iop_stop_pc = argc >= 8 ? parse_address(argv[7]) : 0u;
  const std::uint64_t sbus_probe_step = argc >= 9
      ? std::strtoull(argv[8], nullptr, 0) : 0u;
  const std::uint64_t timer5_probe_step = argc >= 10
      ? std::strtoull(argv[9], nullptr, 0) : 0u;

  ps2vita::Emulator emulator;
  if (!emulator.load_bios(bios.data(), bios.size()) || !emulator.boot_bios()) {
    std::fprintf(stderr, "BIOS must be exactly 4 MiB\n");
    return 2;
  }

  constexpr std::size_t kTraceSize = 256;
  std::array<TraceEntry, kTraceSize> trace{};
  std::array<IopTraceEntry, kTraceSize> iop_trace{};
  std::array<CacheEntry, kTraceSize> cache_trace{};
  std::array<StoreEntry, kTraceSize> low_store_trace{};
  std::array<StoreEntry, kTraceSize> syscall_store_trace{};
  std::array<StoreEntry, kTraceSize> sbus_store_trace{};
  std::array<StoreEntry, kTraceSize> dma_store_trace{};
  std::array<StoreEntry, kTraceSize> gif_store_trace{};
  std::array<StoreEntry, kTraceSize> iop_low_store_trace{};
  std::array<StoreEntry, kTraceSize> iop_sbus_store_trace{};
  std::array<StoreEntry, kTraceSize> iop_dma_store_trace{};
  std::array<StoreEntry, kTraceSize> iop_cdvd_trace{};
  std::array<StoreEntry, kTraceSize> iop_sio2_trace{};
  std::array<StoreEntry, kTraceSize> iop_scmd_trace{};
  std::array<std::uint64_t, 256> iop_scmd_counts{};
  std::array<std::uint64_t, 4096> ee_opcode_counts{};
  std::array<std::uint64_t, 4096> iop_opcode_counts{};
  std::uint64_t ee_profile_total = 0;
  std::uint64_t iop_profile_total = 0;
  std::size_t cursor = 0;
  std::size_t iop_cursor = 0;
  std::size_t cache_cursor = 0;
  std::size_t low_store_cursor = 0;
  std::size_t syscall_store_cursor = 0;
  std::size_t sbus_store_cursor = 0;
  std::size_t dma_store_cursor = 0;
  std::size_t gif_store_cursor = 0;
  std::size_t iop_low_store_cursor = 0;
  std::size_t iop_sbus_store_cursor = 0;
  std::size_t iop_dma_store_cursor = 0;
  std::size_t iop_cdvd_cursor = 0;
  std::size_t iop_sio2_cursor = 0;
  std::size_t iop_scmd_cursor = 0;
  unsigned last_iop_scmd = 256u;
  std::vector<char> serial_output;
  serial_output.reserve(16384);
  std::vector<char> iop_serial_output;
  iop_serial_output.reserve(16384);
  std::vector<StoreEntry> mailbox_store_trace;
  std::vector<StoreEntry> ee_packet_state_store_trace;
  std::vector<Sif0Event> sif0_events;
  std::vector<SyscallEvent> syscall_events;
  constexpr std::array<std::uint32_t, 3> kGraphicsDmaChcr = {
      0x10008000u, 0x10009000u, 0x1000A000u};
  constexpr std::array<const char*, 3> kGraphicsDmaName = {
      "VIF0", "VIF1", "GIF"};
  std::array<std::uint64_t, 3> graphics_dma_starts{};
  std::array<std::uint64_t, 3> graphics_dma_first_step{};
  std::array<std::uint32_t, 3> graphics_dma_first_pc{};
  bool sif0_was_active = false;
  std::uint64_t steps = 0;
  std::uint64_t hits = 0;
  std::uint32_t low_stub_word = 0;
  bool low_stub_seen = false;
  bool low_clear_triggered = false;
  bool iop_stop_triggered = false;
  ps2vita::StopReason reason = ps2vita::StopReason::None;
  ps2vita::IopStopReason iop_reason = ps2vita::IopStopReason::None;
  for (; steps < max_steps; ++steps) {
    if (sbus_probe_step != 0u && steps == sbus_probe_step) {
      std::fprintf(stderr,
          "diagnostic: injecting IOP ICFG bit-1 SBUS probe at step %llu\n",
          static_cast<unsigned long long>(steps));
      emulator.memory().iop_write32(0x1F801450u, 2u);
    }
    if (timer5_probe_step != 0u && steps == timer5_probe_step) {
      const auto target = emulator.memory().iop_read32(0x1F8014A8u);
      std::fprintf(stderr,
          "diagnostic: advancing IOP Timer 5 to target %08X at step %llu\n",
          target, static_cast<unsigned long long>(steps));
      emulator.memory().iop_write32(0x1F8014A0u, target - 1u);
    }
    const auto& state = emulator.cpu().state();
    const bool sif0_active =
        (emulator.memory().iop_read32(0x1F801528u) & 0x01000000u) != 0u;
    if (sif0_active != sif0_was_active) {
      const auto tadr =
          emulator.memory().iop_read32(0x1F80152Cu) & 0x00FFFFFCu;
      const auto iop_tag = emulator.memory().iop_read32(tadr);
      sif0_events.push_back({steps, sif0_active, tadr,
          iop_tag & 0x00FFFFFFu,
          emulator.memory().iop_read32(tadr + 4u) & 0x000FFFFFu,
          emulator.memory().iop_read32(tadr + 8u),
          emulator.memory().iop_read32(tadr + 12u) & 0x0FFFFFF0u});
      sif0_was_active = sif0_active;
    }
    const auto instruction = emulator.memory().read32(state.pc);
    if ((instruction & 0xFC00003Fu) == 0x0000000Cu) {
      syscall_events.push_back({steps, state.pc, state.gpr[3], state.gpr[4],
                                state.gpr[31], state.gpr[29]});
    }
    const auto ee_lines = emulator.memory().ee_interrupt_lines();
    const auto ee_status = state.cop0[12];
    const bool ee_takes_interrupt = ee_lines != 0u &&
        (ee_status & ee_lines) != 0u &&
        (ee_status & 0x00010001u) == 0x00010001u &&
        (ee_status & 0x00000006u) == 0u;
    if (!ee_takes_interrupt) {
      ++ee_opcode_counts[opcode_family(instruction)];
      ++ee_profile_total;
    }
    trace[cursor++ % kTraceSize] = {
        state.pc, instruction, state.gpr[2],
        state.gpr[4], state.gpr[31]};
    if ((instruction >> 26) == 0x2Fu) {
      const unsigned base = (instruction >> 21) & 31u;
      const auto offset = static_cast<std::int16_t>(instruction);
      cache_trace[cache_cursor++ % kTraceSize] = {
          state.pc, instruction,
          static_cast<std::uint32_t>(state.gpr[base] + offset), state.cop0[12]};
    }
    const unsigned opcode = instruction >> 26;
    if (opcode == 0x28u) {
      const unsigned base = (instruction >> 21) & 31u;
      const unsigned source = (instruction >> 16) & 31u;
      const auto offset = static_cast<std::int16_t>(instruction);
      const auto address = static_cast<std::uint32_t>(state.gpr[base] + offset);
      const auto physical = ((address & 0xE0000000u) == 0x80000000u ||
                             (address & 0xE0000000u) == 0xA0000000u)
                                ? address & 0x1FFFFFFFu : address;
      if (physical == 0x1000F180u && serial_output.size() < 16384u)
        serial_output.push_back(static_cast<char>(state.gpr[source]));
    }
    if ((opcode >= 0x28u && opcode <= 0x2Eu) || opcode == 0x1Fu ||
        opcode == 0x3Fu) {
      const unsigned base = (instruction >> 21) & 31u;
      const unsigned source = (instruction >> 16) & 31u;
      const auto offset = static_cast<std::int16_t>(instruction);
      const auto address = static_cast<std::uint32_t>(state.gpr[base] + offset);
      const auto physical = ((address & 0xE0000000u) == 0x80000000u ||
                             (address & 0xE0000000u) == 0xA0000000u)
                                ? address & 0x1FFFFFFFu : address;
      if (physical < 0x2000u) {
        low_store_trace[low_store_cursor++ % kTraceSize] = {
            state.pc, instruction, address, state.gpr[source],
            state.gpr_hi[source]};
      }
      if (physical >= 0x1C0003C0u && physical < 0x1C000420u) {
        mailbox_store_trace.push_back({state.pc, instruction, address,
            state.gpr[source], state.gpr_hi[source]});
      }
      if ((physical >= 0x00023E20u && physical < 0x00023E70u) ||
          (physical >= 0x00024100u && physical < 0x00024140u) ||
          (physical >= 0x000242E0u && physical < 0x00024320u) ||
          (physical >= 0x00024510u && physical < 0x00024540u)) {
        ee_packet_state_store_trace.push_back({state.pc, instruction, address,
            state.gpr[source], state.gpr_hi[source]});
      }
      if (physical >= 0x600u && physical < 0x800u) {
        syscall_store_trace[syscall_store_cursor++ % kTraceSize] = {
            state.pc, instruction, address, state.gpr[source],
            state.gpr_hi[source]};
      }
      if (physical >= 0x1000F200u && physical <= 0x1000F260u) {
        sbus_store_trace[sbus_store_cursor++ % kTraceSize] = {
            state.pc, instruction, address, state.gpr[source],
            state.gpr_hi[source]};
      }
      if ((physical >= 0x10008000u && physical < 0x1000E100u) ||
          (physical >= 0x1000E000u && physical < 0x1000E100u) ||
          (physical >= 0x1000F000u && physical < 0x1000F020u)) {
        dma_store_trace[dma_store_cursor++ % kTraceSize] = {
            state.pc, instruction, address, state.gpr[source],
            state.gpr_hi[source]};
      }
      if ((physical >= 0x10003000u && physical < 0x10003100u) ||
          (physical >= 0x1000A000u && physical < 0x1000A100u) ||
          (physical >= 0x12000000u && physical < 0x12002000u)) {
        gif_store_trace[gif_store_cursor++ % kTraceSize] = {
            state.pc, instruction, address, state.gpr[source],
            state.gpr_hi[source]};
      }
      for (std::size_t channel = 0; channel < kGraphicsDmaChcr.size();
           ++channel) {
        if (physical == kGraphicsDmaChcr[channel] &&
            (state.gpr[source] & 0x100u) != 0u) {
          if (graphics_dma_starts[channel]++ == 0u) {
            graphics_dma_first_step[channel] = steps;
            graphics_dma_first_pc[channel] = state.pc;
          }
        }
      }
    }
    if (stop_pc != 0u && state.pc == stop_pc && ++hits >= stop_hit) break;
    reason = emulator.cpu().step();
    if (reason != ps2vita::StopReason::None) break;
    if ((steps % iop_divisor) == iop_divisor - 1u) {
      const auto& iop = emulator.iop().state();
      const auto iop_instruction = emulator.memory().iop_read32(iop.pc);
      const bool iop_takes_interrupt = emulator.memory().iop_interrupt_pending() &&
          (iop.cop0[12] & 0x00000401u) == 0x00000401u;
      if (!iop_takes_interrupt) {
        ++iop_opcode_counts[opcode_family(iop_instruction)];
        ++iop_profile_total;
      }
      iop_trace[iop_cursor++ % kTraceSize] = {iop.pc, iop_instruction,
          iop.gpr[2], iop.gpr[3], iop.gpr[4], iop.gpr[31]};
      if (iop_stop_pc != 0u && iop.pc == iop_stop_pc) {
        iop_stop_triggered = true;
        break;
      }
      if ((iop_instruction >> 26) == 0x28u) {
        const unsigned base = (iop_instruction >> 21) & 31u;
        const unsigned source = (iop_instruction >> 16) & 31u;
        const auto offset = static_cast<std::int16_t>(iop_instruction);
        const auto address = (iop.gpr[base] + offset) & 0x1FFFFFFFu;
        if (address == 0x1F801040u && iop_serial_output.size() < 16384u)
          iop_serial_output.push_back(static_cast<char>(iop.gpr[source]));
      }
      const unsigned iop_opcode = iop_instruction >> 26;
      if (iop_opcode >= 0x20u && iop_opcode <= 0x26u) {
        const unsigned base = (iop_instruction >> 21) & 31u;
        const auto offset = static_cast<std::int16_t>(iop_instruction);
        const auto address = (iop.gpr[base] + offset) & 0x1FFFFFFFu;
        if (address >= 0x1F402000u && address < 0x1F402100u) {
          iop_cdvd_trace[iop_cdvd_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address, 0u, 0u};
        }
        if (address >= 0x1F808200u && address < 0x1F808280u) {
          iop_sio2_trace[iop_sio2_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address,
              emulator.memory().iop_read32(address & ~3u), 0u};
        }
      }
      if (iop_opcode == 0x28u || iop_opcode == 0x29u ||
          iop_opcode == 0x2Au || iop_opcode == 0x2Bu ||
          iop_opcode == 0x2Eu) {
        const unsigned base = (iop_instruction >> 21) & 31u;
        const unsigned source = (iop_instruction >> 16) & 31u;
        const auto offset = static_cast<std::int16_t>(iop_instruction);
        const auto address = (iop.gpr[base] + offset) & 0x1FFFFFFFu;
        if (address < 0x2000u) {
          iop_low_store_trace[iop_low_store_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address, iop.gpr[source], 0u};
        }
        if (address >= 0x3C0u && address < 0x420u) {
          mailbox_store_trace.push_back({iop.pc, iop_instruction, address,
              iop.gpr[source], 0u});
        }
        if (address >= 0x1D000000u && address <= 0x1D000060u) {
          iop_sbus_store_trace[iop_sbus_store_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address, iop.gpr[source], 0u};
        }
        if (address >= 0x1F402000u && address < 0x1F402100u) {
          iop_cdvd_trace[iop_cdvd_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address, iop.gpr[source], 0u};
          if (address == 0x1F402016u) {
            const auto command = iop.gpr[source] & 0xFFu;
            ++iop_scmd_counts[command];
            if (command != last_iop_scmd) {
              iop_scmd_trace[iop_scmd_cursor++ % kTraceSize] = {
                  iop.pc, iop_instruction, address, command, 0u};
              last_iop_scmd = command;
            }
          }
        }
        if (address >= 0x1F808200u && address < 0x1F808280u) {
          iop_sio2_trace[iop_sio2_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address, iop.gpr[source], 0u};
        }
        if ((address >= 0x1F801070u && address < 0x1F801080u) ||
            (address >= 0x1F8010F0u && address < 0x1F801100u) ||
            (address >= 0x1F801100u && address < 0x1F801130u) ||
            (address >= 0x1F801450u && address < 0x1F801458u) ||
            (address >= 0x1F801480u && address < 0x1F8014B0u) ||
            (address >= 0x1F801520u && address < 0x1F801570u) ||
            (address >= 0x1F801570u && address < 0x1F801578u) ||
            (address >= 0x1F80157Cu && address < 0x1F801580u)) {
          iop_dma_store_trace[iop_dma_store_cursor++ % kTraceSize] = {
              iop.pc, iop_instruction, address, iop.gpr[source], 0u};
        }
      }
      iop_reason = emulator.iop().step();
    }
    const auto current_low_stub_word = emulator.memory().read32(0x80000700u);
    if (current_low_stub_word != 0) low_stub_seen = true;
    if (watch_low_clear && low_stub_seen && low_stub_word != 0 &&
        current_low_stub_word == 0) {
      low_clear_triggered = true;
      break;
    }
    low_stub_word = current_low_stub_word;
  }

  const auto& state = emulator.cpu().state();
  const auto& iop_state = emulator.iop().state();
  if (low_clear_triggered)
    std::puts("watch: physical low kernel stub at 80000700 was cleared");
  if (iop_stop_triggered)
    std::printf("watch: IOP reached %08X\n", iop_stop_pc);
  std::printf("steps=%llu reason=%s pc=%08X opcode=%08X v0=%016llX "
              "a0=%016llX ra=%016llX\n",
      static_cast<unsigned long long>(steps), ps2vita::stop_reason_name(reason),
      state.pc, emulator.memory().read32(state.pc),
      static_cast<unsigned long long>(state.gpr[2]),
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[31]));
  std::printf("dmac_enabler=%08X dmac_enablew=%08X mch_ricm=%08X mch_drd=%08X\n",
      emulator.memory().read32(0x1000F520u),
      emulator.memory().read32(0x1000F590u),
      emulator.memory().read32(0x1000F430u),
      emulator.memory().read32(0x1000F440u));
  std::printf("intc_stat=%08X intc_mask=%08X dmac_stat=%08X\n",
      emulator.memory().read32(0x1000F000u),
      emulator.memory().read32(0x1000F010u),
      emulator.memory().read32(0x1000E010u));
  for (std::size_t channel = 0; channel < kGraphicsDmaChcr.size();
       ++channel) {
    const auto base = kGraphicsDmaChcr[channel];
    std::printf("ee_%s chcr=%08X madr=%08X qwc=%08X tadr=%08X "
                "starts=%llu first_step=%llu first_pc=%08X\n",
        kGraphicsDmaName[channel], emulator.memory().read32(base),
        emulator.memory().read32(base + 0x10u),
        emulator.memory().read32(base + 0x20u),
        emulator.memory().read32(base + 0x30u),
        static_cast<unsigned long long>(graphics_dma_starts[channel]),
        static_cast<unsigned long long>(graphics_dma_first_step[channel]),
        graphics_dma_first_pc[channel]);
  }
  std::printf("ee_sif0 chcr=%08X madr=%08X qwc=%08X tadr=%08X\n",
      emulator.memory().read32(0x1000C000u),
      emulator.memory().read32(0x1000C010u),
      emulator.memory().read32(0x1000C020u),
      emulator.memory().read32(0x1000C030u));
  std::printf("ee_sif1 chcr=%08X madr=%08X qwc=%08X tadr=%08X\n",
      emulator.memory().read32(0x1000C400u),
      emulator.memory().read32(0x1000C410u),
      emulator.memory().read32(0x1000C420u),
      emulator.memory().read32(0x1000C430u));
  const auto sif1_tadr = emulator.memory().read32(0x1000C430u);
  std::printf("ee_sif1_tag=%08X %08X %08X %08X\n",
      emulator.memory().read32(sif1_tadr),
      emulator.memory().read32(sif1_tadr + 4u),
      emulator.memory().read32(sif1_tadr + 8u),
      emulator.memory().read32(sif1_tadr + 12u));
  std::printf("ee_sif1_next=%08X %08X %08X %08X\n",
      emulator.memory().read32(sif1_tadr + 16u),
      emulator.memory().read32(sif1_tadr + 20u),
      emulator.memory().read32(sif1_tadr + 24u),
      emulator.memory().read32(sif1_tadr + 28u));
  const auto sif1_madr = emulator.memory().read32(sif1_tadr + 4u) & 0x0FFFFFF0u;
  std::printf("ee_sif1_data=%08X %08X %08X %08X %08X %08X %08X %08X\n",
      emulator.memory().read32(sif1_madr),
      emulator.memory().read32(sif1_madr + 4u),
      emulator.memory().read32(sif1_madr + 8u),
      emulator.memory().read32(sif1_madr + 12u),
      emulator.memory().read32(sif1_madr + 16u),
      emulator.memory().read32(sif1_madr + 20u),
      emulator.memory().read32(sif1_madr + 24u),
      emulator.memory().read32(sif1_madr + 28u));
  std::puts("EE SIF1 chain tags and transfer headers:");
  auto chain_tadr = emulator.memory().read32(0x1000C430u) & 0x0FFFFFF0u;
  for (unsigned index = 0; index < 16u; ++index, chain_tadr += 16u) {
    const auto tag = emulator.memory().read32(chain_tadr);
    const auto source = emulator.memory().read32(chain_tadr + 4u) & 0x0FFFFFF0u;
    std::printf("%02u tag@%08X=%08X %08X %08X %08X src=%08X "
                "header=%08X %08X %08X %08X\n",
        index, chain_tadr, tag,
        emulator.memory().read32(chain_tadr + 4u),
        emulator.memory().read32(chain_tadr + 8u),
        emulator.memory().read32(chain_tadr + 12u), source,
        emulator.memory().read32(source),
        emulator.memory().read32(source + 4u),
        emulator.memory().read32(source + 8u),
        emulator.memory().read32(source + 12u));
    if (((tag >> 28) & 7u) == 0u) break;
  }
  std::printf("timer3_count=%08X mode=%08X compare=%08X hold=%08X\n",
      emulator.memory().read32(0x10001800u),
      emulator.memory().read32(0x10001810u),
      emulator.memory().read32(0x10001820u),
      emulator.memory().read32(0x10001830u));
  std::printf("sbus_f200=%08X f210=%08X f220=%08X f230=%08X f240=%08X f260=%08X\n",
      emulator.memory().read32(0x1000F200u),
      emulator.memory().read32(0x1000F210u),
      emulator.memory().read32(0x1000F220u),
      emulator.memory().read32(0x1000F230u),
      emulator.memory().read32(0x1000F240u),
      emulator.memory().read32(0x1000F260u));
  std::printf("status=%08X cause=%08X epc=%08X badvaddr=%08X count=%08X\n",
      state.cop0[12], state.cop0[13], state.cop0[14], state.cop0[8],
      state.cop0[9]);
  std::printf("s0=%016llX s1=%016llX s2=%016llX s6=%016llX sp=%016llX\n",
      static_cast<unsigned long long>(state.gpr[16]),
      static_cast<unsigned long long>(state.gpr[17]),
      static_cast<unsigned long long>(state.gpr[18]),
      static_cast<unsigned long long>(state.gpr[22]),
      static_cast<unsigned long long>(state.gpr[29]));
  std::printf("a0=%016llX a1=%016llX a2=%016llX a3=%016llX\n",
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[5]),
      static_cast<unsigned long long>(state.gpr[6]),
      static_cast<unsigned long long>(state.gpr[7]));
  std::printf("v1=%016llX t0=%016llX t1=%016llX\n",
      static_cast<unsigned long long>(state.gpr[3]),
      static_cast<unsigned long long>(state.gpr[8]),
      static_cast<unsigned long long>(state.gpr[9]));
  std::printf("iop_cycles=%llu iop_reason=%s iop_pc=%08X iop_opcode=%08X "
              "iop_v0=%08X iop_sp=%08X iop_ra=%08X iop_status=%08X\n",
      static_cast<unsigned long long>(iop_state.cycles),
      ps2vita::iop_stop_reason_name(iop_reason), iop_state.pc,
      emulator.memory().iop_read32(iop_state.pc), iop_state.gpr[2],
      iop_state.gpr[29], iop_state.gpr[31], iop_state.cop0[12]);
  std::printf("iop_s0=%08X s1=%08X s2=%08X s8=%08X gp=%08X\n",
      iop_state.gpr[16], iop_state.gpr[17], iop_state.gpr[18],
      iop_state.gpr[30], iop_state.gpr[28]);
  std::printf("iop_intc_stat=%08X mask=%08X ctrl=%08X "
              "t0_count=%08X mode=%08X target=%08X cache_ctrl=%08X\n",
      emulator.memory().iop_read32(0x1F801070u),
      emulator.memory().iop_read32(0x1F801074u),
      emulator.memory().iop_read32(0x1F801078u),
      emulator.memory().iop_read32(0x1F801100u),
      emulator.memory().iop_read32(0x1F801104u),
      emulator.memory().iop_read32(0x1F801108u),
      emulator.memory().iop_read32(0x1FFE0130u));
  std::printf("iop_t3 count=%08X mode=%08X target=%08X "
              "t4=%08X/%08X/%08X t5=%08X/%08X/%08X\n",
      emulator.memory().iop_read32(0x1F801480u),
      emulator.memory().iop_read32(0x1F801484u),
      emulator.memory().iop_read32(0x1F801488u),
      emulator.memory().iop_read32(0x1F801490u),
      emulator.memory().iop_read32(0x1F801494u),
      emulator.memory().iop_read32(0x1F801498u),
      emulator.memory().iop_read32(0x1F8014A0u),
      emulator.memory().iop_read32(0x1F8014A4u),
      emulator.memory().iop_read32(0x1F8014A8u));
  std::printf("iop_sif0 madr=%08X bcr=%08X chcr=%08X tadr=%08X "
              "sif1_madr=%08X bcr=%08X chcr=%08X\n",
      emulator.memory().iop_read32(0x1F801520u),
      emulator.memory().iop_read32(0x1F801524u),
      emulator.memory().iop_read32(0x1F801528u),
      emulator.memory().iop_read32(0x1F80152Cu),
      emulator.memory().iop_read32(0x1F801530u),
      emulator.memory().iop_read32(0x1F801534u),
      emulator.memory().iop_read32(0x1F801538u));
  const auto sif0_tadr = emulator.memory().iop_read32(0x1F80152Cu);
  const auto sif0_madr =
      emulator.memory().iop_read32(sif0_tadr) & 0x00FFFFFFu;
  std::printf("iop_sif0_tag=%08X %08X %08X %08X %08X %08X\n",
      emulator.memory().iop_read32(sif0_tadr),
      emulator.memory().iop_read32(sif0_tadr + 4u),
      emulator.memory().iop_read32(sif0_tadr + 8u),
      emulator.memory().iop_read32(sif0_tadr + 12u),
      emulator.memory().iop_read32(sif0_tadr + 16u),
      emulator.memory().iop_read32(sif0_tadr + 20u));
  std::printf("iop_sif0_data=%08X %08X %08X %08X %08X %08X %08X %08X\n",
      emulator.memory().iop_read32(sif0_madr),
      emulator.memory().iop_read32(sif0_madr + 4u),
      emulator.memory().iop_read32(sif0_madr + 8u),
      emulator.memory().iop_read32(sif0_madr + 12u),
      emulator.memory().iop_read32(sif0_madr + 16u),
      emulator.memory().iop_read32(sif0_madr + 20u),
      emulator.memory().iop_read32(sif0_madr + 24u),
      emulator.memory().iop_read32(sif0_madr + 28u));
  std::printf("IOP SIF0 activity transitions: %zu\n", sif0_events.size());
  const auto sif0_begin =
      sif0_events.size() > 64u ? sif0_events.size() - 64u : 0u;
  for (std::size_t i = sif0_begin; i < sif0_events.size(); ++i) {
    const auto& event = sif0_events[i];
    std::printf("%03zu step=%llu %s tadr=%08X source=%08X words=%05X "
                "ee_tag=%08X destination=%08X\n",
        i, static_cast<unsigned long long>(event.step),
        event.active ? "start" : "done ", event.tadr, event.source,
        event.words, event.ee_tag, event.destination);
  }
  std::puts("EE RAM 00100000 staging window:");
  for (std::uint32_t offset = 0u; offset < 0x100u; offset += 16u) {
    std::printf("%08X  %08X %08X %08X %08X\n", 0x00100000u + offset,
        emulator.memory().read32(0x00100000u + offset),
        emulator.memory().read32(0x00100004u + offset),
        emulator.memory().read32(0x00100008u + offset),
        emulator.memory().read32(0x0010000Cu + offset));
  }
  std::printf("EE syscall events: %zu\n", syscall_events.size());
  const auto syscall_begin =
      syscall_events.size() > 96u ? syscall_events.size() - 96u : 0u;
  for (std::size_t i = syscall_begin; i < syscall_events.size(); ++i) {
    const auto& event = syscall_events[i];
    std::printf("%03zu step=%llu pc=%08X number=%016llX a0=%016llX "
                "ra=%016llX sp=%016llX\n",
        i, static_cast<unsigned long long>(event.step), event.pc,
        static_cast<unsigned long long>(event.number),
        static_cast<unsigned long long>(event.a0),
        static_cast<unsigned long long>(event.ra),
        static_cast<unsigned long long>(event.sp));
  }
  std::puts("EE code surrounding final PC:");
  const auto ee_code_base = (state.pc & ~0xFFu) - 0x100u;
  for (std::uint32_t offset = 0u; offset < 0x300u; offset += 16u) {
    const auto address = ee_code_base + offset;
    std::printf("%08X  %08X %08X %08X %08X\n", address,
        emulator.memory().read32(address),
        emulator.memory().read32(address + 4u),
        emulator.memory().read32(address + 8u),
        emulator.memory().read32(address + 12u));
  }
  std::puts("IOP code surrounding final PC:");
  const auto iop_code_base = (iop_state.pc & ~0xFFu) - 0x100u;
  for (std::uint32_t offset = 0u; offset < 0x300u; offset += 16u) {
    const auto address = iop_code_base + offset;
    std::printf("%08X  %08X %08X %08X %08X\n", address,
        emulator.memory().iop_read32(address),
        emulator.memory().iop_read32(address + 4u),
        emulator.memory().iop_read32(address + 8u),
        emulator.memory().iop_read32(address + 12u));
  }
  std::puts("delivered SIF0 packet EE 000935C0 / IOP source 00019870:");
  for (std::uint32_t offset = 0u; offset < 0x40u; offset += 16u) {
    std::printf("EE %08X  %08X %08X %08X %08X   IOP %08X  %08X %08X %08X %08X\n",
        0x000935C0u + offset,
        emulator.memory().read32(0x000935C0u + offset),
        emulator.memory().read32(0x000935C4u + offset),
        emulator.memory().read32(0x000935C8u + offset),
        emulator.memory().read32(0x000935CCu + offset),
        0x00019870u + offset,
        emulator.memory().iop_read32(0x00019870u + offset),
        emulator.memory().iop_read32(0x00019874u + offset),
        emulator.memory().iop_read32(0x00019878u + offset),
        emulator.memory().iop_read32(0x0001987Cu + offset));
  }
  std::puts("EE SIF wait, flag-dispatch, and manager routines:");
  for (const auto base : {0x8000FAC8u, 0x8000FC58u, 0x8000FDE8u,
                          0x80012128u, 0x80012400u, 0x800130D8u,
                          0x80012B00u, 0x800139C0u}) {
    const auto length = base >= 0x80012400u ? 0x500u : 0x180u;
    for (std::uint32_t offset = 0u; offset < length; offset += 16u) {
      std::printf("%08X  %08X %08X %08X %08X\n", base + offset,
          emulator.memory().read32(base + offset),
          emulator.memory().read32(base + offset + 4u),
          emulator.memory().read32(base + offset + 8u),
          emulator.memory().read32(base + offset + 12u));
    }
  }
  std::puts("EE packet/DECI2 live state 80023E00 and 80024300:");
  for (const auto base : {0x80023E00u, 0x80024300u}) {
    for (std::uint32_t offset = 0u; offset < 0x400u; offset += 16u) {
      std::printf("%08X  %08X %08X %08X %08X\n", base + offset,
          emulator.memory().read32(base + offset),
          emulator.memory().read32(base + offset + 4u),
          emulator.memory().read32(base + offset + 8u),
          emulator.memory().read32(base + offset + 12u));
    }
  }
  std::puts("written TLB entries:");
  for (unsigned index = 0; index < 48u; ++index) {
    std::uint32_t mask = 0, hi = 0, lo0 = 0, lo1 = 0;
    if (emulator.memory().read_tlb(index, mask, hi, lo0, lo1))
      std::printf("%02u mask=%08X hi=%08X lo0=%08X lo1=%08X\n",
                  index, mask, hi, lo0, lo1);
  }
  std::puts("low kernel physical memory 80000600..800007FF:");
  for (std::uint32_t address = 0x600u; address < 0x800u; address += 16u) {
    std::printf("%08X  %016llX %016llX\n", 0x80000000u + address,
        static_cast<unsigned long long>(
            emulator.memory().read64(0x80000000u + address)),
        static_cast<unsigned long long>(
            emulator.memory().read64(0x80000008u + address)));
  }
  std::puts("IOP RAM 000003C0..0000041F:");
  for (std::uint32_t address = 0x3C0u; address < 0x420u; address += 16u) {
    std::printf("%08X  %08X %08X %08X %08X\n", address,
        emulator.memory().iop_read32(address),
        emulator.memory().iop_read32(address + 4u),
        emulator.memory().iop_read32(address + 8u),
        emulator.memory().iop_read32(address + 12u));
  }
  std::puts("IOP RAM 00000000..0000017F:");
  for (std::uint32_t address = 0u; address < 0x180u; address += 16u) {
    std::printf("%08X  %08X %08X %08X %08X\n", address,
        emulator.memory().iop_read32(address),
        emulator.memory().iop_read32(address + 4u),
        emulator.memory().iop_read32(address + 8u),
        emulator.memory().iop_read32(address + 12u));
  }
  const std::size_t count = cursor < kTraceSize ? cursor : kTraceSize;
  const std::size_t first = cursor < kTraceSize ? 0 : cursor % kTraceSize;
  std::puts("recent EE instructions:");
  for (std::size_t i = 0; i < count; ++i) {
    const auto& item = trace[(first + i) % kTraceSize];
    std::printf("%08X  %08X  v0=%016llX a0=%016llX ra=%016llX\n",
        item.pc, item.instruction, static_cast<unsigned long long>(item.v0),
        static_cast<unsigned long long>(item.a0),
        static_cast<unsigned long long>(item.ra));
  }
  if (iop_cursor != 0) {
    std::puts("recent IOP instructions:");
    const std::size_t iop_count = iop_cursor < kTraceSize ? iop_cursor : kTraceSize;
    const std::size_t iop_first = iop_cursor < kTraceSize ? 0 : iop_cursor % kTraceSize;
    for (std::size_t i = 0; i < iop_count; ++i) {
      const auto& item = iop_trace[(iop_first + i) % kTraceSize];
      std::printf("%08X  %08X  v0=%08X v1=%08X a0=%08X ra=%08X\n",
          item.pc, item.instruction, item.v0, item.v1, item.a0, item.ra);
    }
  }
  if (cache_cursor != 0) {
    std::puts("recent CACHE operations:");
    const std::size_t cache_count =
        cache_cursor < kTraceSize ? cache_cursor : kTraceSize;
    const std::size_t cache_first =
        cache_cursor < kTraceSize ? 0 : cache_cursor % kTraceSize;
    for (std::size_t i = 0; i < cache_count; ++i) {
      const auto& item = cache_trace[(cache_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X status=%08X\n", item.pc,
                  item.instruction, item.address, item.status);
    }
  }
  if (low_store_cursor != 0) {
    std::puts("recent stores to low RAM:");
    const std::size_t store_count =
        low_store_cursor < kTraceSize ? low_store_cursor : kTraceSize;
    const std::size_t store_first =
        low_store_cursor < kTraceSize ? 0 : low_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = low_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_hi),
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (syscall_store_cursor != 0) {
    std::puts("stores to syscall-stub RAM 00000600..000007FF:");
    const std::size_t store_count =
        syscall_store_cursor < kTraceSize ? syscall_store_cursor : kTraceSize;
    const std::size_t store_first = syscall_store_cursor < kTraceSize
        ? 0 : syscall_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = syscall_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_hi),
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (sbus_store_cursor != 0) {
    std::puts("recent stores to EE SBUS registers:");
    const std::size_t store_count =
        sbus_store_cursor < kTraceSize ? sbus_store_cursor : kTraceSize;
    const std::size_t store_first = sbus_store_cursor < kTraceSize
        ? 0 : sbus_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = sbus_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_hi),
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (gif_store_cursor != 0) {
    std::puts("recent stores to GIF/GS registers:");
    const std::size_t store_count =
        gif_store_cursor < kTraceSize ? gif_store_cursor : kTraceSize;
    const std::size_t store_first =
        gif_store_cursor < kTraceSize ? 0 : gif_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = gif_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n",
                  item.pc, item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_hi),
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (dma_store_cursor != 0) {
    std::puts("recent stores to EE DMA/interrupt registers:");
    const std::size_t store_count =
        dma_store_cursor < kTraceSize ? dma_store_cursor : kTraceSize;
    const std::size_t store_first =
        dma_store_cursor < kTraceSize ? 0 : dma_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = dma_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n",
                  item.pc, item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_hi),
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (iop_low_store_cursor != 0) {
    std::puts("recent IOP stores to low RAM:");
    const std::size_t store_count =
        iop_low_store_cursor < kTraceSize ? iop_low_store_cursor : kTraceSize;
    const std::size_t store_first = iop_low_store_cursor < kTraceSize
        ? 0 : iop_low_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = iop_low_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%08llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (iop_sbus_store_cursor != 0) {
    std::puts("recent IOP stores to SBUS:");
    const std::size_t store_count = iop_sbus_store_cursor < kTraceSize
        ? iop_sbus_store_cursor : kTraceSize;
    const std::size_t store_first = iop_sbus_store_cursor < kTraceSize
        ? 0 : iop_sbus_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = iop_sbus_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%08llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (iop_dma_store_cursor != 0) {
    std::puts("recent IOP stores to DMA/interrupt registers:");
    const std::size_t store_count = iop_dma_store_cursor < kTraceSize
        ? iop_dma_store_cursor : kTraceSize;
    const std::size_t store_first = iop_dma_store_cursor < kTraceSize
        ? 0 : iop_dma_store_cursor % kTraceSize;
    for (std::size_t i = 0; i < store_count; ++i) {
      const auto& item = iop_dma_store_trace[(store_first + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%08llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (iop_sio2_cursor != 0) {
    std::puts("recent IOP SIO2 register accesses:");
    const auto sio2_count = std::min(iop_sio2_cursor, kTraceSize);
    const auto sio2_start = iop_sio2_cursor >= kTraceSize
        ? iop_sio2_cursor % kTraceSize : 0u;
    for (std::size_t i = 0; i < sio2_count; ++i) {
      const auto& item = iop_sio2_trace[(sio2_start + i) % kTraceSize];
      std::printf("%08X  %08X  address=%08X value=%08llX\n", item.pc,
                  item.instruction, item.address,
                  static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (!mailbox_store_trace.empty()) {
    std::puts("EE/IOP low-memory mailbox stores:");
    for (const auto& item : mailbox_store_trace) {
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n",
          item.pc, item.instruction, item.address,
          static_cast<unsigned long long>(item.value_hi),
          static_cast<unsigned long long>(item.value_lo));
    }
  }
  if (!ee_packet_state_store_trace.empty()) {
    std::puts("EE packet/DECI2 state stores:");
    for (const auto& item : ee_packet_state_store_trace) {
      std::printf("%08X  %08X  address=%08X value=%016llX:%016llX\n",
          item.pc, item.instruction, item.address,
          static_cast<unsigned long long>(item.value_hi),
          static_cast<unsigned long long>(item.value_lo));
    }
  }
  std::puts("recent IOP CDVD register accesses:");
  const auto cdvd_count = std::min(iop_cdvd_cursor, kTraceSize);
  const auto cdvd_start = iop_cdvd_cursor >= kTraceSize
      ? iop_cdvd_cursor % kTraceSize : 0u;
  for (std::size_t i = 0; i < cdvd_count; ++i) {
    const auto& entry = iop_cdvd_trace[(cdvd_start + i) % kTraceSize];
    std::printf("%08X  %08X  address=%08X value=%08llX\n",
        entry.pc, entry.instruction, entry.address,
        static_cast<unsigned long long>(entry.value_lo));
  }
  std::puts("IOP CDVD SCMD histogram:");
  for (unsigned command = 0; command < iop_scmd_counts.size(); ++command) {
    if (iop_scmd_counts[command] != 0u)
      std::printf("%02X=%llu ", command,
          static_cast<unsigned long long>(iop_scmd_counts[command]));
  }
  std::putchar('\n');
  std::puts("IOP CDVD SCMD transitions:");
  const auto scmd_count = std::min(iop_scmd_cursor, kTraceSize);
  const auto scmd_start = iop_scmd_cursor >= kTraceSize
      ? iop_scmd_cursor % kTraceSize : 0u;
  for (std::size_t i = 0; i < scmd_count; ++i) {
    const auto& entry = iop_scmd_trace[(scmd_start + i) % kTraceSize];
    std::printf("%08X command=%02llX\n", entry.pc,
        static_cast<unsigned long long>(entry.value_lo));
  }
  print_opcode_profile("EE", ee_opcode_counts, ee_profile_total);
  print_opcode_profile("IOP", iop_opcode_counts, iop_profile_total);
  if (!serial_output.empty()) {
    std::puts("EE serial output:");
    std::fwrite(serial_output.data(), 1, serial_output.size(), stdout);
    if (serial_output.back() != '\n') std::putchar('\n');
  }
  if (!iop_serial_output.empty()) {
    std::puts("IOP serial output:");
    std::fwrite(iop_serial_output.data(), 1, iop_serial_output.size(), stdout);
    if (iop_serial_output.back() != '\n') std::putchar('\n');
  }
  return (stop_pc != 0u && state.pc == stop_pc) || iop_stop_triggered ? 0 : 1;
}
