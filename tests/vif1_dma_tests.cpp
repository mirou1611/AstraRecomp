#include "ps2vita/memory.hpp"
#include "ps2vita/vif.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

unsigned checks = 0;
unsigned failures = 0;

void check(bool condition, const char* name) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", name);
  }
}

constexpr std::uint32_t kChcr = 0x10009000u;
constexpr std::uint32_t kMadr = 0x10009010u;
constexpr std::uint32_t kQwc = 0x10009020u;
constexpr std::uint32_t kTadr = 0x10009030u;
constexpr std::uint32_t kDstat = 0x1000E010u;
constexpr std::uint32_t kTadrSentinel = 0x00004560u;

constexpr std::array<std::uint32_t, 8> kUnpack{{
    0u, 0x01000101u, 0u, 0x6C010123u,
    0x3F800000u, 0xBF000000u, 0x00000001u, 0xDEADBEEFu}};

template <typename Words>
void store_words(ps2vita::Memory& memory, std::uint32_t address,
                 const Words& words) {
  for (const auto word : words) {
    memory.write32(address, word);
    address += 4u;
  }
}

template <typename Words>
bool matches_words(const std::vector<std::uint8_t>& packet,
                   const Words& words) {
  if (packet.size() != words.size() * 4u) return false;
  std::size_t offset = 0;
  for (const auto word : words) {
    for (unsigned byte = 0; byte < 4u; ++byte)
      if (packet[offset++] != ((word >> (byte * 8u)) & 0xFFu)) return false;
  }
  return true;
}

void arm(ps2vita::Memory& memory, std::uint32_t source,
         std::uint32_t qwc, std::uint32_t chcr = 0x101u) {
  memory.write32(0x1000E000u, 1u); // DMA enabled, no MFIFO/stall control.
  memory.write32(kMadr, source);
  memory.write32(kQwc, qwc);
  memory.write32(kTadr, kTadrSentinel);
  memory.write32(kChcr, chcr);
}

void test_ram_and_unpack() {
  ps2vita::Memory memory;
  store_words(memory, 0x2000u, kUnpack);
  arm(memory, 0x2000u, 2u);
  check(memory.cycles_until_next_event() == 1u,
        "armed normal VIF1 DMA is an event boundary");
  memory.advance(1u); // Discover the armed transfer.
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet), "normal VIF1 DMA is not immediate");
  // The existing transport model uses eight EE cycles per qword. These
  // assertions preserve that model, not a new PS2 hardware timing golden.
  memory.advance(15u);
  check(!memory.pop_vif1_packet(packet) &&
            (memory.read32(kChcr) & 0x100u) != 0u,
        "normal VIF1 DMA stays queued until its modeled QWC deadline");
  memory.advance(1u);
  const bool queued = memory.pop_vif1_packet(packet);
  check(queued && matches_words(packet, kUnpack),
        "normal VIF1 DMA emits exactly MADR/QWC bytes without a DMAtag");
  check(memory.read32(kMadr) == 0x2020u && memory.read32(kQwc) == 0u &&
            (memory.read32(kChcr) & 0x100u) == 0u &&
            (memory.read32(kDstat) & (1u << 1)) != 0u,
        "normal VIF1 completion advances MADR, clears QWC/STR and raises CIS1");
  check(memory.read32(kTadr) == kTadrSentinel,
        "normal VIF1 DMA does not consume or modify TADR");
  const auto& spans = memory.vif_dma_spans();
  check(spans.size() == 1u && spans[0].source == 0x2000u &&
            spans[0].stream_offset == 0u && spans[0].bytes == 32u,
        "normal RAM VIF provenance maps the complete raw source span");
  ps2vita::Vif1 vif(memory);
  check(queued && vif.submit(packet.data(), packet.size()),
        "normal DMA payload is a usable VIF stream");
  bool uploaded = queued;
  for (unsigned lane = 0; lane < 4u; ++lane)
    uploaded = uploaded && memory.vu1_data_word(0x1230u + lane * 4u) ==
        kUnpack[4u + lane];
  check(uploaded && vif.vectors_unpacked() == 1u,
        "normal DMA to VIF preserves all four UNPACK raw operand words");
  check(!memory.pop_vif1_packet(packet),
        "normal VIF1 completion emits its packet exactly once");

  memory.clear();
  store_words(memory, 0x2000u, kUnpack);
  arm(memory, 0x2000u, 2u, 0x141u); // TTE is effective only in source chain.
  memory.advance(1u);
  memory.advance(16u);
  check(memory.pop_vif1_packet(packet) && matches_words(packet, kUnpack),
        "normal VIF1 DMA ignores TTE instead of injecting tag bytes");
  check(memory.read32(kTadr) == kTadrSentinel,
        "normal DMA with TTE set still leaves TADR untouched");
}

void test_scratchpad_wrap() {
  ps2vita::Memory memory;
  const auto scratch = ps2vita::Memory::kScratchBase;
  for (unsigned word = 0; word < kUnpack.size(); ++word)
    memory.write32(scratch + ((0x3FF0u + word * 4u) & 0x3FFFu), kUnpack[word]);
  arm(memory, 0x80003FF0u, 2u);
  memory.advance(1u);
  memory.advance(16u);
  std::vector<std::uint8_t> packet;
  check(memory.pop_vif1_packet(packet) && matches_words(packet, kUnpack),
        "normal MADR SPR bit selects scratchpad and wraps reads at 16 KiB");
  check(memory.read32(kMadr) == 0x80004010u &&
            memory.read32(kTadr) == kTadrSentinel &&
            memory.read32(kQwc) == 0u &&
            (memory.read32(kChcr) & 0x100u) == 0u &&
            (memory.read32(kDstat) & (1u << 1)) != 0u,
        "normal scratchpad completion preserves encoded MADR while advancing bytes");
  const auto& spans = memory.vif_dma_spans();
  check(spans.size() == 2u && spans[0].source == scratch + 0x3FF0u &&
            spans[0].stream_offset == 0u && spans[0].bytes == 16u &&
            spans[1].source == scratch && spans[1].stream_offset == 16u &&
            spans[1].bytes == 16u,
        "normal scratchpad provenance splits wrapped CPU-visible source spans");
}

void test_fail_closed_and_reset() {
  ps2vita::Memory memory;
  std::vector<std::uint8_t> packet;
  store_words(memory, 0x2000u, kUnpack);
  arm(memory, 0x2000u, 2u, 0x100u); // DIR=0 is GS download, not forward VIF.
  memory.advance(1u);
  memory.advance(64u);
  check(!memory.pop_vif1_packet(packet) && memory.vif_dma_spans().empty() &&
            memory.read32(kMadr) == 0x2000u && memory.read32(kQwc) == 2u &&
            (memory.read32(kDstat) & (1u << 1)) == 0u,
        "reverse normal VIF1 DMA cannot consume RAM as forward VIF commands");

  memory.clear();
  arm(memory, ps2vita::Memory::kRamSize - 16u, 2u);
  memory.advance(1u);
  memory.advance(64u);
  check(!memory.pop_vif1_packet(packet) && memory.vif_dma_spans().empty() &&
            memory.read32(kMadr) == ps2vita::Memory::kRamSize - 16u &&
            memory.read32(kQwc) == 2u &&
            (memory.read32(kDstat) & (1u << 1)) == 0u,
        "normal VIF1 DMA crossing the valid RAM boundary fails closed");

  memory.clear();
  store_words(memory, 0x2000u, kUnpack);
  arm(memory, 0x2000u, 2u);
  memory.advance(1u);
  memory.advance(7u);
  memory.clear();
  memory.advance(64u);
  check(!memory.pop_vif1_packet(packet) && memory.vif_dma_spans().empty() &&
            (memory.read32(kDstat) & (1u << 1)) == 0u,
        "Memory reset cancels an in-flight normal VIF1 DMA and its provenance");

  memory.clear();
  arm(memory, 0x2000u, 0u);
  memory.advance(1u);
  memory.advance(64u);
  // Sony EE User's Manual v6.0 sections 5.9 (Dn_QWC) and 5.10 explicitly
  // describe normal/interleave QWC=0 kicks as unsuccessful. They do not
  // establish a particular STR/error outcome, so only forbid fake success.
  check(!memory.pop_vif1_packet(packet) && memory.vif_dma_spans().empty() &&
            memory.read32(kMadr) == 0x2000u &&
            memory.read32(kTadr) == kTadrSentinel &&
            (memory.read32(kDstat) & (1u << 1)) == 0u,
        "zero-QWC normal VIF1 kick does not manufacture a successful transfer");
}

void test_live_payload_and_sequential_kicks() {
  ps2vita::Memory memory;
  std::array<std::uint32_t, 4> first{{
      0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u}};
  store_words(memory, 0x2000u, first);
  store_words(memory, 0x3000u, kUnpack);
  arm(memory, 0x2000u, 1u);
  memory.advance(1u);
  // This is a consistency check for the documented model's live copy at
  // completion, not a hardware assertion about DMA sampling or register edits.
  first[1] = 0xA1B2C3D4u;
  memory.write32(0x2004u, first[1]);
  memory.advance(7u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet),
        "payload edit does not bypass the normal DMA modeled deadline");
  memory.advance(1u);
  check(memory.read32(kMadr) == 0x2010u && memory.read32(kQwc) == 0u &&
            (memory.read32(kChcr) & 0x100u) == 0u,
        "first sequential normal kick completes its own source and count");

  // Leave the first completion queued while a second, differently sized
  // normal transfer is discovered and completed, without resetting Memory.
  memory.write32(kDstat, 1u << 1); // Acknowledge the first channel completion.
  arm(memory, 0x3000u, 2u);
  memory.advance(1u);
  memory.advance(15u);
  check(memory.read32(kMadr) == 0x3000u && memory.read32(kQwc) == 2u &&
            (memory.read32(kChcr) & 0x100u) != 0u &&
            (memory.read32(kDstat) & (1u << 1)) == 0u,
        "second sequential normal kick uses a fresh count and deadline");
  memory.advance(1u);
  check(memory.pop_vif1_packet(packet) && matches_words(packet, first),
        "normal DMA completion samples live edited bytes and preserves queue order");
  check(memory.pop_vif1_packet(packet) && matches_words(packet, kUnpack) &&
            !memory.pop_vif1_packet(packet),
        "second normal kick queues only its own payload exactly once");
  const auto& spans = memory.vif_dma_spans();
  check(memory.read32(kMadr) == 0x3020u && memory.read32(kQwc) == 0u &&
            (memory.read32(kChcr) & 0x100u) == 0u &&
            (memory.read32(kDstat) & (1u << 1)) != 0u &&
            memory.read32(kTadr) == kTadrSentinel && spans.size() == 1u &&
            spans[0].source == 0x3000u && spans[0].stream_offset == 0u &&
            spans[0].bytes == 32u,
        "sequential normal completion replaces latched source and provenance");
}

void test_source_state_after_scratchpad() {
  ps2vita::Memory memory;
  const auto scratch = ps2vita::Memory::kScratchBase;
  const std::array<std::uint32_t, 4> scratch_words{{
      0x13579BDFu, 0x2468ACE0u, 0x31415926u, 0x27182818u}};
  store_words(memory, scratch + 0x100u, scratch_words);
  store_words(memory, 0x2000u, kUnpack);
  arm(memory, 0x80000100u, 1u);
  memory.advance(1u);
  memory.advance(8u);
  std::vector<std::uint8_t> packet;
  check(memory.pop_vif1_packet(packet) && matches_words(packet, scratch_words),
        "scratchpad predecessor completes before a normal RAM successor");
  arm(memory, 0x2000u, 2u);
  memory.advance(1u);
  memory.advance(16u);
  check(memory.pop_vif1_packet(packet) && matches_words(packet, kUnpack) &&
            memory.read32(kMadr) == 0x2020u &&
            memory.vif_dma_spans().size() == 1u &&
            memory.vif_dma_spans()[0].source == 0x2000u,
        "successive scratchpad to RAM kick cannot reuse the old SPR source/count");

  arm(memory, 0x80000100u, 1u);
  memory.advance(1u);
  memory.advance(3u);
  memory.clear();
  store_words(memory, 0x3000u, kUnpack);
  arm(memory, 0x3000u, 2u);
  memory.advance(1u);
  memory.advance(16u);
  check(memory.pop_vif1_packet(packet) && matches_words(packet, kUnpack) &&
            !memory.pop_vif1_packet(packet) && memory.read32(kMadr) == 0x3020u &&
            memory.vif_dma_spans().size() == 1u &&
            memory.vif_dma_spans()[0].source == 0x3000u,
        "reset during scratchpad DMA cannot leak its latched state into new RAM DMA");
}

void test_masked_count_and_aligned_source() {
  ps2vita::Memory memory;
  store_words(memory, 0x2000u, kUnpack);
  arm(memory, 0x2007u, 0xDEAD0002u);
  memory.advance(1u);
  memory.advance(15u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet),
        "masked QWC and aligned MADR retain the two-qword modeled deadline");
  memory.advance(1u);
  check(memory.pop_vif1_packet(packet) && matches_words(packet, kUnpack),
        "normal DMA ignores QWC upper bits and aligns the MADR low nibble");
  const auto& spans = memory.vif_dma_spans();
  check(memory.read32(kMadr) == 0x2020u && memory.read32(kQwc) == 0u &&
            memory.read32(kTadr) == kTadrSentinel && spans.size() == 1u &&
            spans[0].source == 0x2000u && spans[0].stream_offset == 0u &&
            spans[0].bytes == 32u,
        "aligned source and effective low-16 QWC determine writeback and spans");
}

void test_non_ram_mappings_fail_closed() {
  // These addresses are CPU-readable according to Memory::valid, but are not
  // RAM or encoded SPR DMA sources. This guards the current support boundary,
  // not a claim about the PS2 DMAC's exact bus-error behavior.
  for (const auto source : {ps2vita::Memory::kNullBase,
                           ps2vita::Memory::kHwBase,
                           ps2vita::Memory::kVu1DataBase,
                           ps2vita::Memory::kScratchBase}) {
    ps2vita::Memory memory;
    check(memory.valid(source, 32u),
          "non-RAM rejection fixture is otherwise a valid CPU mapping");
    arm(memory, source, 2u);
    memory.advance(1u);
    memory.advance(64u);
    std::vector<std::uint8_t> packet;
    check(!memory.pop_vif1_packet(packet) && memory.vif_dma_spans().empty() &&
              memory.read32(kMadr) == source && memory.read32(kQwc) == 2u &&
              memory.read32(kTadr) == kTadrSentinel &&
              (memory.read32(kDstat) & (1u << 1)) == 0u,
          "CPU-valid non-RAM mapping is not consumed as a forward DMA source");
  }
}

std::vector<std::uint32_t> owned_div_packet() {
  // One independently owned case from make_vu1_math_fixture.py: no BIOS or
  // reference-emulator instructions copied. Loads and store have four-pair
  // spacing; WAITQ and its MULq consumer are separate instruction pairs.
  constexpr std::uint32_t lower_nop = 0x8000033Cu;
  constexpr std::uint32_t upper_nop = 0x000002FFu;
  std::array<std::array<std::uint32_t, 2>, 13> program{};
  for (auto& pair : program) pair = {{lower_nop, upper_nop}};
  program[0] = {{(15u << 21) | (1u << 16) | 0x100u,
                 (15u << 21) | (3u << 6) | 0x2Cu}};
  program[1][0] = (15u << 21) | (2u << 16) | 0x101u;
  program[5][0] = 0x800003BCu | (2u << 16) | (1u << 11); // DIV Q,vf1.x,vf2.x
  program[6][0] = 0x800003BFu; // WAITQ
  program[7][1] = (1u << 21) | (3u << 6) | 0x1Cu; // MULq.w vf3,vf0
  program[11] = {{(1u << 25) | (15u << 21) | (3u << 11),
                  upper_nop | 0x40000000u}}; // SQ vf3,0(vi0), E bit
  std::vector<std::uint32_t> words{
      0x10000000u, 0x01000101u, 0x05000000u, 0u,
      0u, 0u, 0u, 0x6C020100u,
      0x3F800000u, 0x3F800000u, 0x3F800000u, 0x3F800000u,
      0x40400000u, 0x40400000u, 0x40400000u, 0x40400000u,
      0u, 0x4A0D0000u};
  for (const auto& pair : program) {
    words.push_back(pair[0]);
    words.push_back(pair[1]);
  }
  words.insert(words.end(), {0x14000000u, 0x10000000u, 0u, 0u});
  return words;
}

void test_owned_finite_vif_run() {
  ps2vita::Memory memory;
  const auto words = owned_div_packet();
  check(words.size() % 4u == 0u, "owned normal DMA math stream is qword aligned");
  store_words(memory, 0x4000u, words);
  arm(memory, 0x4000u, static_cast<std::uint32_t>(words.size() / 4u));
  memory.advance(1u);
  memory.advance(static_cast<std::uint32_t>(words.size() / 4u) * 8u);
  std::vector<std::uint8_t> packet;
  const bool queued = memory.pop_vif1_packet(packet);
  check(queued && matches_words(packet, words),
        "normal DMA preserves the complete owned finite VIF microprogram stream");
  ps2vita::Vif1 vif(memory);
  check(queued && vif.submit(packet.data(), packet.size()) &&
            vif.packets_rejected() == 0u && !vif.vu1().running(),
        "normal DMA finite stream uploads, runs and ends VU1 normally");
  // Arithmetic golden independently observed in both PCSX2 v2.8.2 execution
  // modes at VU1Roundmode=3; not physical PS2 or original timing equivalence.
  check(queued && memory.vu1_data_word(0u) == 0u &&
            memory.vu1_data_word(4u) == 0u && memory.vu1_data_word(8u) == 0u &&
            memory.vu1_data_word(12u) == 0x3EAAAAAAu,
        "normal DMA VIF run stores the observed finite 1/3 raw output");
}

} // namespace

int main() {
  test_ram_and_unpack();
  test_scratchpad_wrap();
  test_fail_closed_and_reset();
  test_live_payload_and_sequential_kicks();
  test_source_state_after_scratchpad();
  test_masked_count_and_aligned_source();
  test_non_ram_mappings_fail_closed();
  test_owned_finite_vif_run();
  std::printf("vif1_dma_tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
