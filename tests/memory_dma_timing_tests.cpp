#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

unsigned checks = 0, failures = 0;
constexpr std::array<std::uint32_t, 4> kGifWords{{
    0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u}};
constexpr std::array<std::uint32_t, 4> kVifWords{{
    0xDEADBEEFu, 0x01234567u, 0x89ABCDEFu, 0x76543210u}};
constexpr std::array<std::uint32_t, 4> kOldWords{{
    0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u}};

void check(bool condition, const std::string& description) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

void write_words(ps2vita::Memory& memory, std::uint32_t address,
                 const std::array<std::uint32_t, 4>& values) {
  for (unsigned word = 0; word < values.size(); ++word)
    memory.write32(address + word * 4u, values[word]);
}

bool packet_is(const std::vector<std::uint8_t>& packet,
               const std::array<std::uint32_t, 4>& expected) {
  if (packet.size() != 16u) return false;
  for (unsigned word = 0; word < expected.size(); ++word)
    for (unsigned byte = 0; byte < 4u; ++byte)
      if (packet[word * 4u + byte] !=
          static_cast<std::uint8_t>(expected[word] >> (byte * 8u))) return false;
  return true;
}

void check_vif_completion(ps2vita::Memory& memory, std::uint32_t final_address,
                          std::uint32_t payload_source, const std::string& label) {
  std::vector<std::uint8_t> packet;
  const bool queued = memory.pop_vif1_packet(packet);
  check(queued && packet_is(packet, kVifWords), label + " queues completion-walk payload");
  check(memory.read32(0x10009010u) == final_address,
        label + " writes completion-walk MADR");
  check(memory.read32(0x10009030u) == final_address,
        label + " writes completion-walk TADR");
  const auto& spans = memory.vif_dma_spans();
  check(spans.size() == 1u && spans[0].source == payload_source &&
            spans[0].stream_offset == 0u && spans[0].bytes == 16u,
        label + " provenance identifies the actually queued source");
  check(memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u &&
            (memory.read32(0x1000E010u) & (1u << 1)) != 0u,
        label + " retires QWC/STR and raises channel 1 status");
  check(!memory.pop_vif1_packet(packet), label + " queues exactly once");
  memory.advance(64u);
  check(!memory.pop_vif1_packet(packet), label + " remains terminal after further ticks");
}

// These are consistency regressions for the existing completion-snapshot DMA
// model, not claims about when real hardware may reread a DMA tag. QWC remains
// unchanged, so neither case asks for a new latency or a bus-rate adjustment.
void test_changed_refe_source_uses_completion_metadata() {
  ps2vita::Memory memory;
  memory.write64(0x2000u, (0x3000ull << 32) | 1u); // REFE, one external qword.
  write_words(memory, 0x3000u, kOldWords);
  write_words(memory, 0x3400u, kVifWords);
  memory.write32(0x10009030u, 0x2000u);
  memory.write32(0x10009000u, 0x105u);
  memory.advance(1u); // Existing model discovers its eight-cycle transfer.
  memory.write64(0x2000u, (0x3400ull << 32) | 1u);
  memory.advance(7u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) &&
            (memory.read32(0x10009000u) & 0x100u) != 0u,
        "edited REFE keeps its original QWC deadline");
  memory.advance(1u);
  const bool queued = memory.pop_vif1_packet(packet);
  check(queued && packet_is(packet, kVifWords), "edited REFE queues the new source bytes");
  check(memory.read32(0x10009010u) == 0x3410u,
        "edited REFE MADR follows queued source, not discovery prediction");
  check(memory.read32(0x10009030u) == 0x2010u,
        "edited REFE leaves the independently fixed end TADR");
  const auto& spans = memory.vif_dma_spans();
  check(spans.size() == 1u && spans[0].source == 0x3400u &&
            spans[0].stream_offset == 0u && spans[0].bytes == 16u,
        "edited REFE source span matches new bytes");
  check(memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u &&
            (memory.read32(0x1000E010u) & (1u << 1)) != 0u,
        "edited REFE reaches normal completion status");
  memory.advance(64u);
  check(!memory.pop_vif1_packet(packet), "edited REFE does not replay after completion");
}

void test_changed_next_target_uses_completion_metadata() {
  ps2vita::Memory memory;
  memory.write64(0x2000u, (0x2200ull << 32) | 0x20000000u); // NEXT, no data.
  memory.write64(0x2200u, 0x70000001u); // Old END, one inline qword.
  memory.write64(0x2400u, 0x70000001u); // New END, same QWC/deadline.
  write_words(memory, 0x2210u, kOldWords);
  write_words(memory, 0x2410u, kVifWords);
  memory.write32(0x10009030u, 0x2000u);
  memory.write32(0x10009000u, 0x105u);
  memory.advance(1u);
  memory.write64(0x2000u, (0x2400ull << 32) | 0x20000000u);
  memory.advance(8u);
  // New END tag at 2400, data at 2410, both final registers at 2420.
  check_vif_completion(memory, 0x2420u, 0x2410u, "edited NEXT");
}

enum class SifChannel { Sif0, Sif1 };

std::string name(SifChannel channel) {
  return channel == SifChannel::Sif0 ? "SIF0" : "SIF1";
}

unsigned deadline(SifChannel channel) {
  // Preserve the existing model's rates: SIF0 eight words * eight cycles;
  // SIF1 four qwords * eight cycles. These are not new hardware constants.
  return channel == SifChannel::Sif0 ? 64u : 32u;
}

void arm_sif(ps2vita::Memory& memory, SifChannel channel) {
  if (channel == SifChannel::Sif0) {
    memory.iop_write32(0x200Cu, 0xC0003000u); // IOP end, source 3000.
    memory.iop_write32(0x2010u, 8u);
    memory.iop_write32(0x2014u, 0x90000002u); // EE CNT/IRQ, QWC2, TIE set.
    memory.iop_write32(0x2018u, 0x6000u);
    for (unsigned word = 0; word < 8u; ++word)
      memory.iop_write32(0x3000u + word * 4u, 0xA0000000u + word);
    memory.write32(0x1000C000u, 0x184u);
    memory.iop_write32(0x1F80152Cu, 0x200Cu);
    memory.iop_write32(0x1F801528u, 0x01000701u);
  } else {
    memory.write64(0x4000u, (0x5000ull << 32) | 4u); // REFE, QWC4.
    memory.write32(0x5000u, 0xC0007000u); // SIF destination/end attributes.
    memory.write32(0x5004u, 12u); // One SIF header qword plus 12 data words.
    for (unsigned word = 0; word < 12u; ++word)
      memory.write32(0x5010u + word * 4u, 0xB0000000u + word);
    memory.write32(0x1000C430u, 0x4000u);
    memory.write32(0x1000C400u, 0x184u);
    memory.iop_write32(0x1F801538u, 0x41000300u);
  }
}

bool sif_pending(const ps2vita::Memory& memory, SifChannel channel) {
  return (memory.read32(channel == SifChannel::Sif0 ? 0x1000C000u : 0x1000C400u) &
          0x100u) != 0u &&
         (memory.iop_read32(channel == SifChannel::Sif0 ? 0x1F801528u : 0x1F801538u) &
          0x01000000u) != 0u;
}

std::uint32_t sif_first_word(const ps2vita::Memory& memory, SifChannel channel) {
  return channel == SifChannel::Sif0 ? memory.read32(0x6000u)
                                    : memory.iop_read32(0x7000u);
}

void check_sif_completion(ps2vita::Memory& memory, SifChannel channel) {
  const auto label = name(channel);
  const bool sif0 = channel == SifChannel::Sif0;
  bool payload_matches = true;
  for (unsigned word = 0; word < (sif0 ? 8u : 12u); ++word)
    payload_matches = payload_matches &&
        (sif0 ? memory.read32(0x6000u + word * 4u)
              : memory.iop_read32(0x7000u + word * 4u)) ==
            (sif0 ? 0xA0000000u : 0xB0000000u) + word;
  check(payload_matches, label + " retains exact source payload during concurrent discovery");
  const auto ee_chcr = sif0 ? 0x1000C000u : 0x1000C400u;
  const auto iop_chcr = sif0 ? 0x1F801528u : 0x1F801538u;
  check((memory.read32(ee_chcr) & 0x100u) == 0u &&
            (memory.iop_read32(iop_chcr) & 0x01000000u) == 0u,
        label + " clears both start bits at its own deadline");
  check(memory.read32(ee_chcr + 0x10u) == (sif0 ? 0x6020u : 0x5040u) &&
            memory.read32(ee_chcr + 0x20u) == 0u &&
            memory.iop_read32(sif0 ? 0x1F801520u : 0x1F801530u) ==
                (sif0 ? 0x3020u : 0x7030u),
        label + " writes fixed terminal transfer registers");
  check((sif0 ? memory.iop_read32(0x1F80152Cu) : memory.read32(0x1000C430u)) ==
            (sif0 ? 0x201Cu : 0x4010u),
        label + " writes its fixed terminal tag address");
  check((memory.read32(0x1000E010u) & (1u << (sif0 ? 5u : 6u))) != 0u &&
            (memory.iop_read32(0x1F801574u) & (1u << (sif0 ? 26u : 27u))) != 0u &&
            (memory.iop_read32(0x1F801070u) & (1u << 3)) != 0u,
        label + " raises EE and IOP DMA completion status");
}

void arm_graphics(ps2vita::Memory& memory) {
  write_words(memory, 0x8000u, kGifWords);
  memory.write32(0x1000A010u, 0x8000u);
  memory.write32(0x1000A020u, 1u);
  memory.write32(0x1000A000u, 0x101u);
  memory.write64(0x9000u, 0x70000001u); // END, one inline qword.
  write_words(memory, 0x9010u, kVifWords);
  memory.write32(0x10009030u, 0x9000u);
  memory.write32(0x10009000u, 0x105u);
}

void test_pending_sif_does_not_starve_graphics(SifChannel channel) {
  ps2vita::Memory memory;
  const auto label = name(channel) + " pending tick";
  memory.iop_write32(0x1F8014A4u, 0u); // Independent ungated Timer 5 clock.
  memory.iop_write32(0x1F8014A0u, 0u);
  arm_sif(memory, channel);
  memory.advance(1u);
  check(sif_pending(memory, channel) && sif_first_word(memory, channel) == 0u,
        label + " begins with SIF scheduled but not copied");
  arm_graphics(memory);
  memory.advance(1u); // An unfinished SIF tick must still discover both channels.
  memory.advance(7u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_gif_packet(packet) && !memory.pop_vif1_packet(packet) &&
            (memory.read32(0x1000E010u) & 6u) == 0u,
        label + " keeps graphics quiet until their unchanged eight-cycle deadline");
  memory.advance(1u);
  const bool gif_queued = memory.pop_gif_packet(packet);
  check(gif_queued && packet_is(packet, kGifWords), label + " permits GIF discovery/completion");
  const bool vif_queued = memory.pop_vif1_packet(packet);
  check(vif_queued && packet_is(packet, kVifWords), label + " permits VIF discovery/completion");
  check(memory.read32(0x1000A010u) == 0x8010u &&
            memory.read32(0x1000A020u) == 0u &&
            (memory.read32(0x1000A000u) & 0x100u) == 0u,
        label + " retires GIF registers independently");
  check(memory.read32(0x10009010u) == 0x9020u &&
            memory.read32(0x10009030u) == 0x9020u &&
            memory.read32(0x10009020u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u,
        label + " retires VIF registers independently");
  const auto& spans = memory.vif_dma_spans();
  check(spans.size() == 1u && spans[0].source == 0x9010u && spans[0].bytes == 16u,
        label + " keeps VIF payload provenance");
  check(sif_pending(memory, channel) && sif_first_word(memory, channel) == 0u &&
            (memory.read32(0x1000E010u) & (1u <<
                (channel == SifChannel::Sif0 ? 5u : 6u))) == 0u,
        label + " does not finish SIF early while graphics finish");
  check((memory.read32(0x1000E010u) & 6u) == 6u,
        label + " exposes both independent graphics completion bits");
  check(memory.iop_read32(0x1F8014A0u) == 1u,
        label + " advances Timer 5 on all ten elapsed EE cycles");
  check(!memory.pop_gif_packet(packet) && !memory.pop_vif1_packet(packet),
        label + " consumes each completed graphics packet only once");

  // Nine of SIF's scheduled cycles elapsed after discovery. Stop one cycle
  // before its original deadline, then verify completion on that final cycle.
  memory.advance(deadline(channel) - 10u);
  check(sif_pending(memory, channel) && sif_first_word(memory, channel) == 0u,
        label + " preserves SIF progress up to its last cycle");
  memory.advance(1u);
  check_sif_completion(memory, channel);
  check(memory.iop_read32(0x1F8014A0u) == (deadline(channel) + 1u) / 8u,
        label + " preserves the independent clock phase through SIF completion");
  memory.advance(128u);
  check(!memory.pop_gif_packet(packet) && !memory.pop_vif1_packet(packet) &&
            !sif_pending(memory, channel),
        label + " remains terminal without extra graphics submissions");
}

void test_sif_channels_advance_independently_of_function_order() {
  ps2vita::Memory memory;
  arm_sif(memory, SifChannel::Sif0); // Earlier completion code, longer deadline.
  arm_sif(memory, SifChannel::Sif1); // Later completion code, shorter deadline.
  memory.advance(1u);
  memory.advance(31u);
  check(sif_pending(memory, SifChannel::Sif0) && sif_pending(memory, SifChannel::Sif1) &&
            sif_first_word(memory, SifChannel::Sif0) == 0u &&
            sif_first_word(memory, SifChannel::Sif1) == 0u,
        "both SIF channels remain pending before the shorter deadline");
  memory.advance(1u);
  check_sif_completion(memory, SifChannel::Sif1);
  check(sif_pending(memory, SifChannel::Sif0) &&
            sif_first_word(memory, SifChannel::Sif0) == 0u,
        "later SIF1 code finishes before earlier SIF0 code with a longer deadline");
  memory.advance(31u);
  check(sif_pending(memory, SifChannel::Sif0) &&
            sif_first_word(memory, SifChannel::Sif0) == 0u,
        "SIF0 keeps its own last-cycle boundary after SIF1 completion");
  memory.advance(1u);
  check_sif_completion(memory, SifChannel::Sif0);
}

void test_reset_discards_pending_devices_and_completed_queues() {
  ps2vita::Memory memory;
  arm_sif(memory, SifChannel::Sif0);
  arm_sif(memory, SifChannel::Sif1);
  arm_graphics(memory);
  memory.advance(1u);
  memory.advance(3u);
  memory.clear();
  memory.advance(128u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_gif_packet(packet) && !memory.pop_vif1_packet(packet) &&
            memory.vif_dma_spans().empty(),
        "reset cancels active graphics transfers and provenance");
  check(memory.read32(0x6000u) == 0u && memory.iop_read32(0x7000u) == 0u &&
            (memory.read32(0x1000E010u) & 0x66u) == 0u,
        "reset cancels both pending SIF copies and old completion status");
  check(!sif_pending(memory, SifChannel::Sif0) &&
            !sif_pending(memory, SifChannel::Sif1) &&
            (memory.read32(0x1000A000u) & 0x100u) == 0u &&
            (memory.read32(0x10009000u) & 0x100u) == 0u,
        "reset does not resurrect any prior channel start bit");
  arm_graphics(memory);
  memory.advance(1u);
  memory.advance(8u);
  check((memory.read32(0x1000E010u) & 6u) == 6u &&
            !memory.vif_dma_spans().empty(),
        "fresh graphics transfers still complete after reset");
  memory.clear(); // Leave both successfully queued packets unconsumed.
  memory.advance(128u);
  check(!memory.pop_gif_packet(packet) && !memory.pop_vif1_packet(packet) &&
            memory.vif_dma_spans().empty() &&
            (memory.read32(0x1000E010u) & 6u) == 0u,
        "reset also discards completed queues and their status/provenance");
}

} // namespace

int main() {
  test_changed_refe_source_uses_completion_metadata();
  test_changed_next_target_uses_completion_metadata();
  test_pending_sif_does_not_starve_graphics(SifChannel::Sif0);
  test_pending_sif_does_not_starve_graphics(SifChannel::Sif1);
  test_sif_channels_advance_independently_of_function_order();
  test_reset_discards_pending_devices_and_completed_queues();
  if (failures != 0u) {
    std::fprintf(stderr, "%u of %u Memory DMA timing checks failed\n", failures, checks);
    return 1;
  }
  std::printf("Memory DMA timing tests passed (%u checks)\n", checks);
  return 0;
}
