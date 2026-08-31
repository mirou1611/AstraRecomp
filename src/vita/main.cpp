#include "ps2vita/aot.hpp"
#include "ps2vita/emulator.hpp"
#include "screen.hpp"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr const char* kBootPath = "ux0:data/ps2vita/boot.elf";
constexpr const char* kBiosPath = "ux0:data/ps2vita/bios.bin";
constexpr const char* kProbePath = "ux0:data/ps2vita/startup.txt";
constexpr const char* kFaultPath = "ux0:data/ps2vita/fault.txt";
constexpr const char* kProgressPath = "ux0:data/ps2vita/progress.txt";
constexpr const char* kBenchmarkPath = "ux0:data/ps2vita/benchmark.txt";

std::uint64_t benchmark_clock_microseconds() {
  return sceKernelGetProcessTimeWide();
}

void write_probe(const char* status) {
  sceIoMkdir("ux0:data/ps2vita", 0777);
  const int fd = sceIoOpen(kProbePath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  for (const char* cursor = status; *cursor; ++cursor) sceIoWrite(fd, cursor, 1);
  sceIoClose(fd);
}

void truncate_report(const char* path) {
  // Vita3K's host-backed filesystem has historically retained stale trailing
  // bytes after O_TRUNC. Removing the tiny diagnostic first makes each run's
  // report unambiguous on both Vita hardware and Vita3K.
  sceIoRemove(path);
  const int fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd >= 0) {
    static constexpr char reset[] = "none\n";
    sceIoWrite(fd, reset, sizeof(reset) - 1);
    sceIoClose(fd);
  }
}

void write_fault(const ps2vita::Emulator& emulator, ps2vita::StopReason reason) {
  char report[768];
  const auto& cpu = emulator.cpu();
  const auto& memory = emulator.memory();
  const auto& state = cpu.state();
  const auto read_debug = [&](std::uint32_t address) {
    return memory.valid(address, 4) ? memory.read32(address) : 0xFFFFFFFFu;
  };
  std::snprintf(report, sizeof(report),
      "reason=%s\npc=%08X\nopcode=%08X\ncycles=%llu\n"
      "v0=%016llX\na0=%016llX\nsp=%016llX\nra=%016llX\n"
      "status=%08X\ncause=%08X\nepc=%08X\nbadvaddr=%08X\n"
      "epc_opcode=%08X\nvector180=%08X,%08X,%08X,%08X\n"
      "vector200=%08X,%08X,%08X,%08X\n",
      ps2vita::stop_reason_name(reason), state.pc, cpu.fault_instruction(),
      static_cast<unsigned long long>(state.cycles),
      static_cast<unsigned long long>(state.gpr[2]),
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[29]),
      static_cast<unsigned long long>(state.gpr[31]),
      state.cop0[12], state.cop0[13], state.cop0[14], state.cop0[8],
      read_debug(state.cop0[14]),
      read_debug(0x80000180u), read_debug(0x80000184u),
      read_debug(0x80000188u), read_debug(0x8000018Cu),
      read_debug(0x80000200u), read_debug(0x80000204u),
      read_debug(0x80000208u), read_debug(0x8000020Cu));
  const int fd = sceIoOpen(kFaultPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, report, std::strlen(report));
  sceIoClose(fd);
}

void write_progress(const ps2vita::Emulator& emulator) {
  char report[256];
  const auto& state = emulator.cpu().state();
  const auto opcode = emulator.memory().read32(state.pc);
  std::snprintf(report, sizeof(report),
      "pc=%08X\nopcode=%08X\ncycles=%llu\nfast_path_instructions=%llu\n"
      "v0=%016llX\na0=%016llX\nsp=%016llX\nra=%016llX\n"
      "status=%08X\ncause=%08X\nepc=%08X\nbadvaddr=%08X\n",
      state.pc, opcode, static_cast<unsigned long long>(state.cycles),
      static_cast<unsigned long long>(state.fast_path_instructions),
      static_cast<unsigned long long>(state.gpr[2]),
      static_cast<unsigned long long>(state.gpr[4]),
      static_cast<unsigned long long>(state.gpr[29]),
      static_cast<unsigned long long>(state.gpr[31]), state.cop0[12],
      state.cop0[13], state.cop0[14], state.cop0[8]);
  const int fd = sceIoOpen(kProgressPath, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, report, std::strlen(report));
  sceIoClose(fd);
}

void write_benchmark(const ps2vita::AotBenchmarkResult& result) {
  char report[768];
  std::snprintf(report, sizeof(report),
      "schema=astrart-performance-v1\nmatched=%u\niterations=%u\nsamples=%u\n"
      "guest_instructions=%llu\ninterpreter_us=%llu\nnative_us=%llu\n"
      "speedup_x100=%u\ninterpreter_checksum=%016llX\n"
      "native_checksum=%016llX\n"
      "timing_note=Vita3K validates correctness only; physical Vita decides performance\n",
      result.matched ? 1u : 0u, result.iterations, result.samples,
      static_cast<unsigned long long>(result.guest_instructions),
      static_cast<unsigned long long>(result.interpreter_microseconds),
      static_cast<unsigned long long>(result.aot_microseconds),
      result.speedup_x100,
      static_cast<unsigned long long>(result.interpreter_checksum),
      static_cast<unsigned long long>(result.aot_checksum));
  sceIoRemove(kBenchmarkPath);
  const int fd = sceIoOpen(kBenchmarkPath,
                           SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0666);
  if (fd < 0) return;
  sceIoWrite(fd, report, std::strlen(report));
  sceIoClose(fd);
}

void put16(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint16_t value) {
  bytes[at] = static_cast<std::uint8_t>(value);
  bytes[at + 1] = static_cast<std::uint8_t>(value >> 8);
}

void put32(std::vector<std::uint8_t>& bytes, std::size_t at, std::uint32_t value) {
  put16(bytes, at, static_cast<std::uint16_t>(value));
  put16(bytes, at + 2, static_cast<std::uint16_t>(value >> 16));
}

std::vector<std::uint8_t> builtin_smoke_elf() {
  std::vector<std::uint8_t> bytes(0x10C, 0);
  bytes[0] = 0x7F; bytes[1] = 'E'; bytes[2] = 'L'; bytes[3] = 'F';
  bytes[4] = 1; bytes[5] = 1; bytes[6] = 1;
  put16(bytes, 16, 2); put16(bytes, 18, 8); put32(bytes, 20, 1);
  put32(bytes, 24, 0x1000); put32(bytes, 28, 52);
  put16(bytes, 40, 52); put16(bytes, 42, 32); put16(bytes, 44, 1);
  put32(bytes, 52, 1); put32(bytes, 56, 0x100); put32(bytes, 60, 0x1000);
  put32(bytes, 64, 0x1000); put32(bytes, 68, 12); put32(bytes, 72, 16);
  put32(bytes, 76, 5); put32(bytes, 80, 16);
  put32(bytes, 0x100, 0x2402002A); // addiu v0,zero,42
  put32(bytes, 0x104, 0xAC022000); // sw v0,0x2000(zero)
  put32(bytes, 0x108, 0x0000000D); // break
  return bytes;
}

bool read_file(const char* path, std::vector<std::uint8_t>& bytes) {
  const int fd = sceIoOpen(path, SCE_O_RDONLY, 0);
  if (fd < 0) return false;
  const auto size = sceIoLseek(fd, 0, SCE_SEEK_END);
  sceIoLseek(fd, 0, SCE_SEEK_SET);
  if (size <= 0 || size > 32 * 1024 * 1024) { sceIoClose(fd); return false; }
  bytes.resize(static_cast<std::size_t>(size));
  std::size_t done = 0;
  while (done < bytes.size()) {
    const int got = sceIoRead(fd, bytes.data() + done, bytes.size() - done);
    if (got <= 0) { sceIoClose(fd); return false; }
    done += static_cast<std::size_t>(got);
  }
  sceIoClose(fd);
  return true;
}

void draw(Screen& screen, const ps2vita::Emulator& emu, const char* message,
          const ps2vita::AotProbeResult& aot_probe,
          const ps2vita::AotProbeResult& aot_chain_probe,
          const ps2vita::AotBenchmarkResult& benchmark, bool running,
          bool show_gs) {
  constexpr std::uint32_t bg = 0xFF100C18;
  constexpr std::uint32_t panel = 0xFF21182E;
  constexpr std::uint32_t ink = 0xFFF4ECFF;
  constexpr std::uint32_t dim = 0xFFB7A8C8;
  constexpr std::uint32_t accent = 0xFFFF4E9A;
  if (show_gs) {
    screen.clear(0xFF000000);
    screen.blit_160x112(emu.gs().pixels());
    screen.rect(0, 0, 960, 40, 0xFF100C18);
    screen.text(18, 10, "GS REFERENCE 160X112 / R TO RETURN", ink, 2);
    screen.present();
    return;
  }
  screen.clear(bg);
  screen.rect(0, 0, 960, 70, accent);
  screen.text(28, 19, "ASTRARECOMP / EE MONITOR", 0xFF180C13, 3);
  screen.rect(28, 96, 904, 274, panel);
  screen.text(52, 116, message, ink, 2);

  char line[96];
  const auto& cpu = emu.cpu().state();
  std::snprintf(line, sizeof(line), "STATE: %s", running ? "RUNNING" : ps2vita::stop_reason_name(emu.cpu().stop_reason()));
  screen.text(52, 164, line, running ? 0xFF6CFFB5 : accent, 2);
  std::snprintf(line, sizeof(line), "PC: %08X    CYCLES: %llu", cpu.pc,
                static_cast<unsigned long long>(cpu.cycles));
  screen.text(52, 196, line, ink, 2);
  std::snprintf(line, sizeof(line), "V0: %08X    A0: %08X    SP: %08X",
                static_cast<unsigned>(cpu.gpr[2]), static_cast<unsigned>(cpu.gpr[4]),
                static_cast<unsigned>(cpu.gpr[29]));
  screen.text(52, 228, line, dim, 2);
  std::snprintf(line, sizeof(line), "NATIVE FAST: %llu INSTRUCTIONS",
                static_cast<unsigned long long>(cpu.fast_path_instructions));
  screen.text(52, 260, line, dim, 2);
  std::snprintf(line, sizeof(line), "PS2R AOT T0: %s   INTERP:%u   NATIVE:%u",
                aot_probe.matched ? "PASS" : "FAIL",
                aot_probe.interpreter_value, aot_probe.aot_value);
  screen.text(52, 292, line, aot_probe.matched ? 0xFF6CFFB5 : accent, 2);
  std::snprintf(line, sizeof(line), "PS2R AOT T1 CALL/RET: %s   NATIVE:%u",
                aot_chain_probe.matched ? "PASS" : "FAIL",
                aot_chain_probe.aot_value);
  screen.text(52, 316, line,
              aot_chain_probe.matched ? 0xFF6CFFB5 : accent, 2);

  std::snprintf(line, sizeof(line), "AOT PERF: %s  %u.%02uX  %llu GUEST OPS",
                benchmark.matched ? "PASS" : "FAIL",
                benchmark.speedup_x100 / 100u, benchmark.speedup_x100 % 100u,
                static_cast<unsigned long long>(benchmark.guest_instructions));
  screen.text(52, 344, line, benchmark.matched ? 0xFF6CFFB5 : accent, 2);

  screen.text(38, 387, "CROSS  RUN / PAUSE", ink, 2);
  screen.text(38, 419, "SQUARE STEP    TRIANGLE RELOAD", ink, 2);
  screen.text(38, 451, "CIRCLE BIOS   R GS VIEW   START RESET", ink, 2);
  screen.text(38, 503, "BENCHMARK.TXT / VITA3K CORRECTNESS GATE", dim, 2);
  screen.present();
}
}

int main() {
  write_probe("main entered\n");
  truncate_report(kFaultPath);
  truncate_report(kProgressPath);
  Screen screen;
  if (!screen.init()) return -1;
  write_probe("screen ready\n");
  sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

  ps2vita::Emulator emulator;
  const auto aot_probe = ps2vita::run_phase0_aot_probe();
  const auto aot_chain_probe = ps2vita::run_phase0_aot_chain_probe();
  write_probe("running AOT performance validation\n");
  const auto benchmark = ps2vita::run_phase0_aot_benchmark(
      benchmark_clock_microseconds, 4096u, 5u);
  write_benchmark(benchmark);
  write_probe(benchmark.matched ? "AOT benchmark passed\n" :
                                  "AOT benchmark failed\n");
  std::vector<std::uint8_t> image;
  char message[96];
  std::snprintf(message, sizeof(message), "PHASE-0 AOT %s / I=%u A=%u",
                aot_probe.matched ? "PASS" : "FAIL",
                aot_probe.interpreter_value, aot_probe.aot_value);
  bool running = false;
  bool show_gs = false;
  std::uint32_t previous = 0;
  std::uint64_t next_progress_cycle = 1000000;

  emulator.gs().clear(0xFF120A20);
  emulator.gs().triangle({8, 100, 200, 0xFFFF4E9A},
                         {80, 8, 100, 0xFF6CFFB5},
                         {152, 100, 200, 0xFFFFC857});
  emulator.gs().line({8, 104, 50, 0xFFFFFFFF}, {152, 104, 50, 0xFFFFFFFF});

  image = builtin_smoke_elf();
  const auto initial = emulator.load_elf(image.data(), image.size());
  if (!initial.ok)
    std::snprintf(message, sizeof(message), "BUILT-IN LOAD ERROR: %s", initial.error);

  std::vector<std::uint8_t> bios;
  if (read_file(kBiosPath, bios) && emulator.load_bios(bios.data(), bios.size()) &&
      emulator.boot_bios()) {
    running = true;
    std::snprintf(message, sizeof(message), "BIOS AUTOLOADED / RUNNING");
  }

  while (true) {
    SceCtrlData pad{};
    sceCtrlPeekBufferPositive(0, &pad, 1);
    const std::uint32_t pressed = pad.buttons & ~previous;
    previous = pad.buttons;

    if (pressed & SCE_CTRL_SELECT) break;
    if (pressed & SCE_CTRL_RTRIGGER) show_gs = !show_gs;
    if (pressed & SCE_CTRL_TRIANGLE) {
      running = false;
      image.clear();
      bool loaded_external = read_file(kBootPath, image);
      if (!loaded_external) {
        image = builtin_smoke_elf();
      }
      if (image.empty()) {
        std::snprintf(message, sizeof(message), "COULD NOT READ ANY ELF");
      } else {
        const auto result = emulator.load_elf(image.data(), image.size());
        if (result.ok) std::snprintf(message, sizeof(message), "%s / ENTRY %08X",
            loaded_external ? "EXTERNAL ELF LOADED" : "BUILT-IN TEST LOADED", result.entry);
        else std::snprintf(message, sizeof(message), "LOAD ERROR: %s", result.error);
      }
    }
    if (pressed & SCE_CTRL_CIRCLE) {
      running = false;
      image.clear();
      if (!read_file(kBiosPath, image)) {
        std::snprintf(message, sizeof(message), "COULD NOT READ BIOS.BIN");
      } else if (!emulator.load_bios(image.data(), image.size())) {
        std::snprintf(message, sizeof(message), "BIOS MUST BE EXACTLY 4 MIB");
      } else {
        emulator.boot_bios();
        std::snprintf(message, sizeof(message), "BIOS MAPPED / RESET BFC00000");
      }
    }
    if ((pressed & SCE_CTRL_CROSS) && emulator.ready()) running = !running;
    if ((pressed & SCE_CTRL_START) && emulator.ready()) { emulator.reset(); running = false; }
    if ((pressed & SCE_CTRL_SQUARE) && emulator.ready()) {
      const auto reason = emulator.cpu().step();
      running = false;
      if (reason != ps2vita::StopReason::None) {
        write_fault(emulator, reason);
        std::snprintf(message, sizeof(message), "STOPPED: %s", ps2vita::stop_reason_name(reason));
      }
    }
    if (running) {
      const auto reason = emulator.run_slice(25000);
      if (emulator.cpu().state().cycles >= next_progress_cycle) {
        write_progress(emulator);
        next_progress_cycle = emulator.cpu().state().cycles + 1000000;
      }
      if (reason != ps2vita::StopReason::StepLimit) {
        running = false;
        write_fault(emulator, reason);
        std::snprintf(message, sizeof(message), "STOPPED: %s", ps2vita::stop_reason_name(reason));
      }
    }
    draw(screen, emulator, message, aot_probe, aot_chain_probe, benchmark,
         running, show_gs);
  }

  screen.shutdown();
  sceKernelExitProcess(0);
  return 0;
}
