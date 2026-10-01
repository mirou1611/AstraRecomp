#include "ps2vita/memory.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

unsigned checks = 0, failures = 0;
constexpr std::uint32_t kChcr = 0x10009000u;
constexpr std::uint32_t kMadr = 0x10009010u;
constexpr std::uint32_t kQwc = 0x10009020u;
constexpr std::uint32_t kTadr = 0x10009030u;
constexpr std::uint32_t kDstat = 0x1000E010u;
constexpr std::uint32_t kTadrSentinel = 0x00004560u;
constexpr std::array<std::uint32_t, 8> kPhysical{{
    0x11223344u, 0x55667788u, 0x99AABBCCu, 0xDDEEFF00u,
    0x13579BDFu, 0x2468ACE0u, 0x31415926u, 0x27182818u}};
constexpr std::array<std::uint32_t, 8> kTranslated{{
    0xA1A2A3A4u, 0xB1B2B3B4u, 0xC1C2C3C4u, 0xD1D2D3D4u,
    0xE1E2E3E4u, 0xF1F2F3F4u, 0xABABCDEFu, 0xCDCD0123u}};

void check(bool condition, const std::string& label) {
  ++checks;
  if (!condition) {
    ++failures;
    std::fprintf(stderr, "FAIL: %s\n", label.c_str());
  }
}

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
  for (const auto word : words)
    for (unsigned byte = 0; byte < 4u; ++byte)
      if (packet[offset++] != ((word >> (byte * 8u)) & 0xFFu)) return false;
  return true;
}

void redirect_cpu_pair(ps2vita::Memory& memory, unsigned index,
                       std::uint32_t virtual_pair,
                       std::uint32_t physical_pair) {
  // Two ordinary, valid, writable 4 KiB pages. DMA must not consult these
  // CPU mappings even if its physical address happens to match EntryHi.
  memory.write_tlb(index, 0u, virtual_pair,
                   ((physical_pair >> 12) << 6) | 7u,
                   (((physical_pair + 0x1000u) >> 12) << 6) | 7u);
}

void arm_normal(ps2vita::Memory& memory, std::uint32_t source) {
  memory.write32(0x1000E000u, 1u);
  memory.write32(kMadr, source);
  memory.write32(kQwc, 2u);
  memory.write32(kTadr, kTadrSentinel);
  memory.write32(kChcr, 0x101u);
}

void arm_chain(ps2vita::Memory& memory, std::uint32_t tag,
               bool transfer_tag = false) {
  memory.write32(0x1000E000u, 1u);
  memory.write32(kMadr, 0u);
  memory.write32(kQwc, 0u);
  memory.write32(kTadr, tag);
  memory.write32(kChcr, transfer_tag ? 0x145u : 0x105u);
}

template <typename Words>
void expect_completion(ps2vita::Memory& memory, const Words& expected,
                       std::uint32_t final_madr, std::uint32_t final_tadr,
                       std::initializer_list<ps2vita::VifDmaSpan> expected_spans,
                       const std::string& label) {
  memory.advance(1u); // Discover the transfer in the current transport model.
  memory.advance(15u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) &&
            (memory.read32(kChcr | 0xA0000000u) & 0x100u) != 0u &&
            (memory.read32(kDstat | 0xA0000000u) & 2u) == 0u,
        label + " retains its existing two-qword modeled deadline");
  memory.advance(1u);
  const bool queued = memory.pop_vif1_packet(packet);
  check(queued && matches_words(packet, expected),
        label + " queues physical bytes rather than CPU-translated bytes");
  check(memory.read32(kMadr | 0xA0000000u) == final_madr,
        label + " writes the physical transfer's encoded final MADR");
  check(memory.read32(kTadr | 0xA0000000u) == final_tadr,
        label + " keeps the correct normal or chain final TADR");
  check(memory.read32(kQwc | 0xA0000000u) == 0u &&
            (memory.read32(kChcr | 0xA0000000u) & 0x100u) == 0u &&
            (memory.read32(kDstat | 0xA0000000u) & 2u) != 0u,
        label + " retires QWC/STR and raises channel 1 completion");
  const auto& actual_spans = memory.vif_dma_spans();
  bool spans_match = actual_spans.size() == expected_spans.size();
  std::size_t span_index = 0;
  for (const auto& expected_span : expected_spans) {
    if (span_index < actual_spans.size()) {
      const auto& actual = actual_spans[span_index];
      spans_match = spans_match && actual.source == expected_span.source &&
          actual.stream_offset == expected_span.stream_offset &&
          actual.bytes == expected_span.bytes;
    }
    ++span_index;
  }
  check(spans_match, label + " reports the physical bytes' source provenance");
  memory.advance(64u);
  check(!memory.pop_vif1_packet(packet), label + " completes exactly once");
}

void test_normal_ram_bypasses_cpu_tlb() {
  ps2vita::Memory memory;
  store_words(memory, 0x2000u, kPhysical);
  store_words(memory, 0x6000u, kTranslated);
  redirect_cpu_pair(memory, 0u, 0x2000u, 0x6000u);
  check(memory.read32(0x2000u) == kTranslated[0],
        "normal RAM control proves CPU reads still follow the TLB");
  memory.write32(0x2004u, 0xF00DFACEu);
  check(memory.read32(0x80006004u) == 0xF00DFACEu &&
            memory.read32(0x80002004u) == kPhysical[1],
        "normal RAM control proves CPU writes redirect without editing physical source");
  arm_normal(memory, 0x2000u);
  expect_completion(memory, kPhysical, 0x2020u, kTadrSentinel,
                    {{0x2000u, 0u, 32u}}, "normal physical RAM");
  check(memory.read32(0x2000u) == kTranslated[0],
        "normal DMA does not remove or modify the CPU's TLB mapping");
}

void test_normal_scratch_bypasses_cpu_tlb() {
  ps2vita::Memory memory;
  store_words(memory, ps2vita::Memory::kScratchBase + 0x100u, kPhysical);
  store_words(memory, 0x6100u, kTranslated);
  redirect_cpu_pair(memory, 0u, ps2vita::Memory::kScratchBase, 0x6000u);
  check(memory.read32(ps2vita::Memory::kScratchBase + 0x100u) == kTranslated[0],
        "normal SPR control proves a CPU aperture mapping exists");
  arm_normal(memory, 0x80000100u);
  expect_completion(memory, kPhysical, 0x80000120u, kTadrSentinel,
                    {{ps2vita::Memory::kScratchBase + 0x100u, 0u, 32u}},
                    "normal physical SPR");
}

void test_chain_tag_and_tte_bypass_cpu_tlb() {
  ps2vita::Memory memory;
  const auto physical_tag = (0x4000ull << 32) | 2u; // REFE, two qwords.
  const auto translated_tag = (0x8000ull << 32) | 2u;
  memory.write64(0x2000u, physical_tag);
  memory.write64(0x2008u, 0x3333444411112222ull);
  memory.write64(0x6000u, translated_tag);
  memory.write64(0x6008u, 0xBBBBCCCCAAAADDDDull);
  store_words(memory, 0x4000u, kPhysical);
  store_words(memory, 0x8000u, kTranslated);
  redirect_cpu_pair(memory, 0u, 0x2000u, 0x6000u);
  check(memory.read64(0x2000u) == translated_tag,
        "chain tag control proves CPU tag reads select the decoy chain");
  std::array<std::uint32_t, 10> expected{{0x11112222u, 0x33334444u}};
  for (unsigned i = 0; i < kPhysical.size(); ++i) expected[i + 2u] = kPhysical[i];
  arm_chain(memory, 0x2000u, true);
  expect_completion(memory, expected, 0x4020u, 0x2010u,
                    {{0x2008u, 0u, 8u}, {0x4000u, 8u, 32u}},
                    "physical chain tag/TTE");
}

void test_chain_ram_payload_bypasses_cpu_tlb() {
  ps2vita::Memory memory;
  memory.write64(0x2000u, (0x4000ull << 32) | 2u);
  store_words(memory, 0x4000u, kPhysical);
  store_words(memory, 0x8000u, kTranslated);
  redirect_cpu_pair(memory, 0u, 0x4000u, 0x8000u);
  check(memory.read32(0x4000u) == kTranslated[0],
        "chain payload control proves CPU payload reads select decoy RAM");
  arm_chain(memory, 0x2000u);
  expect_completion(memory, kPhysical, 0x4020u, 0x2010u,
                    {{0x4000u, 0u, 32u}}, "physical chain RAM payload");
}

void test_chain_channel_state_bypasses_cpu_tlb() {
  ps2vita::Memory memory;
  memory.write64(0x2000u, (0x4000ull << 32) | 2u);
  memory.write64(0x6000u, (0x8000ull << 32) | 2u);
  store_words(memory, 0x4000u, kPhysical);
  store_words(memory, 0x8000u, kTranslated);
  // The real channel has TTE clear and points to 2000. The CPU alias sees a
  // decoy CHCR/TADR in RAM. DMA channel state must come from the hardware bank.
  memory.write32(0x7000u, 0x145u);
  memory.write32(0x7030u, 0x6000u);
  arm_chain(memory, 0x2000u);
  redirect_cpu_pair(memory, 0u, 0x10008000u, 0x6000u);
  check(memory.read32(kChcr) == 0x145u && memory.read32(kTadr) == 0x6000u &&
            memory.read32(kChcr | 0xA0000000u) == 0x105u &&
            memory.read32(kTadr | 0xA0000000u) == 0x2000u,
        "channel-state control distinguishes CPU aliases from physical DMA registers");
  expect_completion(memory, kPhysical, 0x4020u, 0x2010u,
                    {{0x4000u, 0u, 32u}}, "physical chain channel state");
}

void test_chain_spr_tag_and_inline_payload_bypass_cpu_tlb() {
  ps2vita::Memory memory;
  const auto scratch = ps2vita::Memory::kScratchBase;
  memory.write64(scratch + 0x100u, 0x70000002u); // END, two inline qwords.
  memory.write64(0x6100u, 0x70000002u);
  store_words(memory, scratch + 0x110u, kPhysical);
  store_words(memory, 0x6110u, kTranslated);
  redirect_cpu_pair(memory, 0u, scratch, 0x6000u);
  check(memory.read32(scratch + 0x110u) == kTranslated[0],
        "chain SPR control proves CPU inline payload reads redirect to RAM");
  arm_chain(memory, 0x80000100u);
  expect_completion(memory, kPhysical, 0x80000130u, 0x80000130u,
                    {{scratch + 0x110u, 0u, 32u}},
                    "physical chain SPR tag/inline payload");
}

void test_chain_existing_non_ram_sources_keep_physical_policy() {
  // These are compatibility checks for source-chain regions already accepted
  // by Memory, not an expansion of normal mode's RAM/SPR support boundary or
  // a claim that every CPU-readable region is accessible on physical PS2 DMA.
  {
    ps2vita::Memory memory;
    const std::array<std::uint32_t, 8> zero_words{};
    memory.write64(0x2000u, (std::uint64_t{ps2vita::Memory::kNullBase} << 32) | 2u);
    check(memory.valid(ps2vita::Memory::kNullBase, 32u),
          "chain null-aperture fixture was already a supported physical region");
    arm_chain(memory, 0x2000u);
    expect_completion(memory, zero_words, ps2vita::Memory::kNullBase + 32u,
                      0x2010u, {{ps2vita::Memory::kNullBase, 0u, 32u}},
                      "existing chain null aperture");
  }
  {
    ps2vita::Memory memory;
    const std::array<std::uint32_t, 8> zero_words{};
    memory.write64(0x2000u,
        (std::uint64_t{ps2vita::Memory::kDevBoardBase} << 32) | 2u);
    check(memory.valid(ps2vita::Memory::kDevBoardBase, 32u) &&
              memory.read32(ps2vita::Memory::kDevBoardBase) == 0u,
          "chain dev-board fixture was already a supported physical zero-read region");
    arm_chain(memory, 0x2000u);
    expect_completion(memory, zero_words, ps2vita::Memory::kDevBoardBase + 32u,
                      0x2010u, {{ps2vita::Memory::kDevBoardBase, 0u, 32u}},
                      "existing chain dev-board aperture");
  }
  {
    ps2vita::Memory memory;
    memory.write64(0x2000u, (std::uint64_t{ps2vita::Memory::kVu1DataBase} << 32) | 2u);
    for (unsigned i = 0; i < kPhysical.size(); ++i)
      memory.vu1_store_data_word(static_cast<std::uint16_t>(i * 4u), kPhysical[i]);
    store_words(memory, 0x8000u, kTranslated);
    redirect_cpu_pair(memory, 0u, ps2vita::Memory::kVu1DataBase, 0x8000u);
    check(memory.read32(ps2vita::Memory::kVu1DataBase) == kTranslated[0],
          "chain VU-memory control proves CPU reads follow an ordinary TLB mapping");
    arm_chain(memory, 0x2000u);
    expect_completion(memory, kPhysical, ps2vita::Memory::kVu1DataBase + 32u,
                      0x2010u, {{ps2vita::Memory::kVu1DataBase, 0u, 32u}},
                      "existing physical chain VU-memory payload");
  }
}

void test_cpu_mapping_cannot_validate_an_unsupported_physical_tag() {
  ps2vita::Memory memory;
  constexpr std::uint32_t unsupported = 0x13000000u;
  memory.write64(0x2000u, 0x70000002u);
  store_words(memory, 0x2010u, kPhysical);
  check(!memory.valid(unsupported, 16u),
        "unmapped chain-tag fixture begins outside the existing physical regions");
  redirect_cpu_pair(memory, 0u, unsupported, 0x2000u);
  check(memory.valid(unsupported, 16u) &&
            memory.read64(unsupported) == 0x70000002u,
        "unsupported physical-tag control proves its CPU alias is readable");
  arm_chain(memory, unsupported);
  memory.advance(1u);
  memory.advance(64u);
  std::vector<std::uint8_t> packet;
  check(!memory.pop_vif1_packet(packet) && memory.vif_dma_spans().empty(),
        "CPU alias cannot turn an unsupported physical chain tag into a packet");
  check(memory.read32(kMadr) == 0u && memory.read32(kQwc) == 0u &&
            memory.read32(kTadr) == unsupported &&
            (memory.read32(kDstat) & 2u) == 0u,
        "rejected physical chain tag cannot publish fake writeback or completion");
}

} // namespace

int main() {
  test_normal_ram_bypasses_cpu_tlb();
  test_normal_scratch_bypasses_cpu_tlb();
  test_chain_tag_and_tte_bypass_cpu_tlb();
  test_chain_ram_payload_bypasses_cpu_tlb();
  test_chain_channel_state_bypasses_cpu_tlb();
  test_chain_spr_tag_and_inline_payload_bypass_cpu_tlb();
  test_chain_existing_non_ram_sources_keep_physical_policy();
  test_cpu_mapping_cannot_validate_an_unsupported_physical_tag();
  std::printf("vif_dma_physical_tests: %u checks, %u failures\n", checks, failures);
  return failures == 0u ? 0 : 1;
}
