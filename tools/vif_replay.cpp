#include "ps2vita/vif.hpp"
#include "ps2vita/gif.hpp"
#include "ps2vita/framebuffer_dump.hpp"
#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: ps2vif_replay FIRST_VIF_BIN FRAMEBUFFER_PPM\n");
    return 2;
  }
  std::ifstream input(argv[1], std::ios::binary | std::ios::ate);
  const auto size = input.tellg();
  if (!input || size <= 0 || size > 1024 * 1024) {
    std::fprintf(stderr, "VIF input must be 1..1048576 bytes\n"); return 2;
  }
  std::vector<std::uint8_t> data(static_cast<std::size_t>(size));
  input.seekg(0);
  if (!input.read(reinterpret_cast<char*>(data.data()), data.size())) return 2;
  ps2vita::Memory memory;
  ps2vita::Vif1 vif(memory);
  vif.vu1().enable_store_trace(true);
  ps2vita::Gs gs;
  ps2vita::Gif gif(gs);
  const bool accepted = vif.submit(data.data(), data.size());
  std::vector<std::uint8_t> packet;
  bool gif_ok = true;
  while (vif.pop_gif_packet(packet))
    gif_ok = gif.submit(packet.data(), packet.size()) && gif_ok;
  const auto& vu = vif.vu1();
  std::printf("accepted=%u pending_direct_bytes=%zu vif_rejected=%llu vu_pairs=%llu path1=%llu/%llu "
              "reject_pc=%04X kick=%04X bad=%04X tag=%016llX triangles=%llu\n",
      static_cast<unsigned>(accepted), vif.pending_direct_bytes(),
      static_cast<unsigned long long>(vif.packets_rejected()),
      static_cast<unsigned long long>(vu.pairs_executed()),
      static_cast<unsigned long long>(vu.path1_tags_queued()),
      static_cast<unsigned long long>(vu.path1_tags_rejected()),
      vu.first_rejected_pc(), vu.first_rejected_kick_start(), vu.first_rejected_address(),
      static_cast<unsigned long long>(vu.first_rejected_tag()),
      static_cast<unsigned long long>(gif.triangles_emitted()));
  std::ofstream image(argv[2], std::ios::binary | std::ios::trunc);
  std::printf("vu_timing cycles=%llu vf_stalls=%llu q_stalls=%llu xgkick_stalls=%llu\n",
      static_cast<unsigned long long>(vu.cycles_executed()),
      static_cast<unsigned long long>(vu.vf_stall_cycles()),
      static_cast<unsigned long long>(vu.q_stall_cycles()),
      static_cast<unsigned long long>(vu.xgkick_stall_cycles()));
  std::printf("store_trace records=%zu dropped=%llu reject_pair=%llu\n",
      vu.store_records().size(), static_cast<unsigned long long>(vu.dropped_store_records()),
      static_cast<unsigned long long>(vu.first_rejected_pair()));
  if (vu.path1_tags_rejected() != 0u) {
    const unsigned span = ((vu.first_rejected_address() - vu.first_rejected_kick_start()) & 0x3FFFu) + 16u;
    for (const auto& record : vu.store_records()) {
      if (((record.address - vu.first_rejected_kick_start()) & 0x3FFFu) >= span) continue;
      std::printf("packet_store pair=%llu cycle=%llu pc=%04X address=%04X value=%08X relation=%s\n",
          static_cast<unsigned long long>(record.pair), static_cast<unsigned long long>(record.cycle), record.pc, record.address, record.value,
          record.pair < vu.first_rejected_pair() ? "before" : "after_or_same");
    }
  }
  const bool written = ps2vita::write_framebuffer_ppm(image, gs);
  image.close();
  if (!written || !image) return 2;
  // Isolated replay starts with reset GS state, not prior BIOS path-3 uploads.
  // Its image is diagnostic, not a replacement for a full-BIOS framebuffer.
  return accepted && gif_ok && vif.pending_direct_bytes() == 0u &&
      vu.path1_tags_rejected() == 0u ? 0 : 1;
}
