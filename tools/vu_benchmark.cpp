#include "ps2vita/vif.hpp"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <vector>

namespace {
std::uint64_t mix(std::uint64_t h, std::uint32_t value) {
  return (h ^ value) * 1099511628211ull;
}

std::uint64_t checksum(const ps2vita::Vu1& vu, const ps2vita::Memory& memory) {
  std::uint64_t h = 1469598103934665603ull;
  const auto& s = vu.state();
  for (const auto& reg : s.vf) for (auto lane : reg) h = mix(h, lane);
  for (auto reg : s.vi) h = mix(h, reg);
  for (auto lane : s.acc) h = mix(h, lane);
  for (auto value : {s.i, s.q, std::uint32_t{s.mac}, std::uint32_t{s.pc}})
    h = mix(h, value);
  for (std::uint32_t offset = 0; offset < 0x4000u; offset += 4u)
    h = mix(h, memory.read32(ps2vita::Memory::kVu1DataBase + offset));
  for (auto value : {vu.pairs_executed(), vu.cycles_executed(),
                     vu.vf_stall_cycles(), vu.q_stall_cycles(),
                     vu.xgkick_stall_cycles(), vu.path1_tags_queued(),
                     vu.path1_tags_rejected()}) {
    h = mix(h, std::uint32_t(value)); h = mix(h, std::uint32_t(value >> 32));
  }
  return h;
}

std::uint64_t gif_checksum(ps2vita::Vu1& vu) {
  std::uint64_t h = 1469598103934665603ull;
  std::vector<std::uint8_t> packet;
  while (vu.pop_path1_packet(packet)) {
    h = mix(h, static_cast<std::uint32_t>(packet.size()));
    for (auto byte : packet) h = mix(h, byte);
  }
  const auto rejected = vu.first_rejected_tag();
  h = mix(h, static_cast<std::uint32_t>(rejected));
  h = mix(h, static_cast<std::uint32_t>(rejected >> 32));
  h = mix(h, vu.first_rejected_address());
  for (auto word : vu.first_rejected_data()) h = mix(h, word);
  return h;
}
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: ps2vu_benchmark CAPTURE.bin REPETITIONS(1..100000)\n");
    return 2;
  }
  char* end = nullptr;
  const auto repetitions = std::strtoul(argv[2], &end, 10);
  if (!argv[2][0] || *end || repetitions == 0 || repetitions > 100000) return 2;
  std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  if (!input || size <= 0 || size > 1024 * 1024 || size % 16) return 2;
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(data.data()), data.size())) return 2;
  ps2vita::Memory memory;
  ps2vita::Vif1 vif(memory);
  std::uint64_t expected = 0;
  std::uint64_t expected_gif = 0;
  double milliseconds = 0.0;
  for (unsigned long repetition = 0; repetition < repetitions; ++repetition) {
    vif.reset();
    memory.zero(ps2vita::Memory::kVu1DataBase, 0x4000u);
    memory.zero(ps2vita::Memory::kVu1MicroBase, 0x4000u);
    const auto start = std::chrono::steady_clock::now();
    const bool accepted = vif.submit(data.data(), data.size());
    const auto finish = std::chrono::steady_clock::now();
    milliseconds += std::chrono::duration<double, std::milli>(finish - start).count();
    if (!accepted) return 1;
    const auto actual = checksum(vif.vu1(), memory);
    const auto actual_gif = gif_checksum(vif.vu1());
    if (repetition && (actual != expected || actual_gif != expected_gif)) {
      std::fprintf(stderr, "non-deterministic VU state at repetition %lu\n", repetition);
      return 1;
    }
    expected = actual;
    expected_gif = actual_gif;
  }
  std::printf("repetitions=%lu pairs_each=%llu cycles_each=%llu checksum=%016llX gif_checksum=%016llX submit_ms=%.3f ns_per_pair=%.2f\n",
      repetitions, static_cast<unsigned long long>(vif.vu1().pairs_executed()),
      static_cast<unsigned long long>(vif.vu1().cycles_executed()),
      static_cast<unsigned long long>(expected),
      static_cast<unsigned long long>(expected_gif), milliseconds,
      milliseconds * 1000000.0 / (repetitions * vif.vu1().pairs_executed()));
  return 0;
}
