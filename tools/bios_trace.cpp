#include "ps2vita/emulator.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
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

} // namespace

int main(int argc, char** argv) {
  if (argc < 2 || argc > 6) {
    std::fprintf(stderr,
        "usage: ps2bios_trace BIOS [STOP_PC] [MAX_STEPS] [STOP_HIT] "
        "[WATCH_LOW_CLEAR]\n");
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

  ps2vita::Emulator emulator;
  if (!emulator.load_bios(bios.data(), bios.size()) || !emulator.boot_bios()) {
    std::fprintf(stderr, "BIOS must be exactly 4 MiB\n");
    return 2;
  }

  constexpr std::size_t kTraceSize = 64;
  std::array<TraceEntry, kTraceSize> trace{};
  std::array<IopTraceEntry, kTraceSize> iop_trace{};
  std::array<CacheEntry, kTraceSize> cache_trace{};
  std::array<StoreEntry, kTraceSize> low_store_trace{};
  std::array<StoreEntry, kTraceSize> syscall_store_trace{};
  std::array<StoreEntry, kTraceSize> sbus_store_trace{};
  std::size_t cursor = 0;
  std::size_t iop_cursor = 0;
  std::size_t cache_cursor = 0;
  std::size_t low_store_cursor = 0;
  std::size_t syscall_store_cursor = 0;
  std::size_t sbus_store_cursor = 0;
  std::vector<char> serial_output;
  serial_output.reserve(16384);
  std::vector<char> iop_serial_output;
  iop_serial_output.reserve(16384);
  std::uint64_t steps = 0;
  std::uint64_t hits = 0;
  std::uint32_t low_stub_word = 0;
  bool low_stub_seen = false;
  bool low_clear_triggered = false;
  ps2vita::StopReason reason = ps2vita::StopReason::None;
  ps2vita::IopStopReason iop_reason = ps2vita::IopStopReason::None;
  for (; steps < max_steps; ++steps) {
    const auto& state = emulator.cpu().state();
    const auto instruction = emulator.memory().read32(state.pc);
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
    }
    if (state.pc == stop_pc && ++hits >= stop_hit) break;
    reason = emulator.cpu().step();
    if (reason != ps2vita::StopReason::None) break;
    if ((steps & 7u) == 7u) {
      const auto& iop = emulator.iop().state();
      const auto iop_instruction = emulator.memory().iop_read32(iop.pc);
      iop_trace[iop_cursor++ % kTraceSize] = {iop.pc, iop_instruction,
          iop.gpr[2], iop.gpr[3], iop.gpr[4], iop.gpr[31]};
      if ((iop_instruction >> 26) == 0x28u) {
        const unsigned base = (iop_instruction >> 21) & 31u;
        const unsigned source = (iop_instruction >> 16) & 31u;
        const auto offset = static_cast<std::int16_t>(iop_instruction);
        const auto address = (iop.gpr[base] + offset) & 0x1FFFFFFFu;
        if (address == 0x1F801040u && iop_serial_output.size() < 16384u)
          iop_serial_output.push_back(static_cast<char>(iop.gpr[source]));
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
  std::printf("steps=%llu reason=%s pc=%08X v0=%016llX a0=%016llX ra=%016llX\n",
      static_cast<unsigned long long>(steps), ps2vita::stop_reason_name(reason),
      state.pc, static_cast<unsigned long long>(state.gpr[2]),
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[31]));
  std::printf("dmac_enabler=%08X dmac_enablew=%08X mch_ricm=%08X mch_drd=%08X\n",
      emulator.memory().read32(0x1000F520u),
      emulator.memory().read32(0x1000F590u),
      emulator.memory().read32(0x1000F430u),
      emulator.memory().read32(0x1000F440u));
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
              "iop_v0=%08X iop_sp=%08X iop_ra=%08X\n",
      static_cast<unsigned long long>(iop_state.cycles),
      ps2vita::iop_stop_reason_name(iop_reason), iop_state.pc,
      emulator.memory().iop_read32(iop_state.pc), iop_state.gpr[2],
      iop_state.gpr[29], iop_state.gpr[31]);
  std::printf("iop_s0=%08X s1=%08X s2=%08X s8=%08X gp=%08X\n",
      iop_state.gpr[16], iop_state.gpr[17], iop_state.gpr[18],
      iop_state.gpr[30], iop_state.gpr[28]);
  std::printf("iop_intc_stat=%08X mask=%08X ctrl=%08X "
              "t0_count=%08X mode=%08X target=%08X\n",
      emulator.memory().iop_read32(0x1F801070u),
      emulator.memory().iop_read32(0x1F801074u),
      emulator.memory().iop_read32(0x1F801078u),
      emulator.memory().iop_read32(0x1F801100u),
      emulator.memory().iop_read32(0x1F801104u),
      emulator.memory().iop_read32(0x1F801108u));
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
  const std::size_t count = cursor < kTraceSize ? cursor : kTraceSize;
  const std::size_t first = cursor < kTraceSize ? 0 : cursor % kTraceSize;
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
  return state.pc == stop_pc ? 0 : 1;
}
