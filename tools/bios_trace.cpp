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
  if (argc < 2 || argc > 8) {
    std::fprintf(stderr,
        "usage: ps2bios_trace BIOS [STOP_PC] [MAX_STEPS] [STOP_HIT] "
        "[WATCH_LOW_CLEAR] [IOP_DIVISOR] [IOP_STOP_PC]\n");
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
  std::size_t iop_scmd_cursor = 0;
  unsigned last_iop_scmd = 256u;
  std::vector<char> serial_output;
  serial_output.reserve(16384);
  std::vector<char> iop_serial_output;
  iop_serial_output.reserve(16384);
  std::uint64_t steps = 0;
  std::uint64_t hits = 0;
  std::uint32_t low_stub_word = 0;
  bool low_stub_seen = false;
  bool low_clear_triggered = false;
  bool iop_stop_triggered = false;
  ps2vita::StopReason reason = ps2vita::StopReason::None;
  ps2vita::IopStopReason iop_reason = ps2vita::IopStopReason::None;
  for (; steps < max_steps; ++steps) {
    const auto& state = emulator.cpu().state();
    const auto instruction = emulator.memory().read32(state.pc);
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
    }
    if (state.pc == stop_pc && ++hits >= stop_hit) break;
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
  std::puts("EE wait/SIF routines 8000FDE8 and 800125C0:");
  for (const auto base : {0x8000FDE8u, 0x800125C0u}) {
    for (std::uint32_t offset = 0u; offset < 0xC0u; offset += 16u) {
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
  return state.pc == stop_pc || iop_stop_triggered ? 0 : 1;
}
