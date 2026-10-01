#include "ps2vita/memory.hpp"
#include "ps2vita/vu.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>

namespace {

unsigned failures = 0;
constexpr std::uint32_t kLowerNop = 0x8000033Cu;
constexpr std::uint32_t kUpperNop = 0x000002FFu;

void check(bool condition, const std::string& description) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", description.c_str());
    ++failures;
  }
}

void pair(ps2vita::Memory& memory, unsigned pc, std::uint32_t lower,
          std::uint32_t upper = kUpperNop) {
  memory.write32(ps2vita::Memory::kVu1MicroBase + pc, lower);
  memory.write32(ps2vita::Memory::kVu1MicroBase + pc + 4u, upper);
}

constexpr std::uint32_t ibne(unsigned is, unsigned it, unsigned displacement) {
  return (0x29u << 25) | (it << 16) | (is << 11) | displacement;
}

constexpr std::uint32_t add_x(unsigned fd, unsigned fs) {
  return (8u << 21) | (fs << 11) | (fd << 6) | 0x28u;
}

// Expectations are derived from these pinned primary sources:
// https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/VUops.cpp
//   _vuBackupVI and its callers; _vuIBNE/_vuIBLEZ consult the saved value.
// https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/VU1microInterp.cpp
//   _vu1Exec ages the two-cycle backup before execution, including VF stalls.
// These tests assert branch destinations and stores, independently of how the
// core represents that history. FMAND deliberately is not a backup caller.
void test_backed_up_writers() {
  struct Writer {
    const char* name;
    std::uint32_t instruction;
    std::uint16_t old_value;
  };
  constexpr std::array<Writer, 9> writers{{
      {"ISUBIU", 0x12031801u, 1u}, // VI3 = VI3 - 1
      {"IADDIU", 0x10030000u, 1u}, // VI3 = VI0 + 0
      {"IADD",   0x800000F0u, 1u}, // VI3 = VI0 + VI0
      {"IADDI",  0x80030032u, 1u}, // VI3 = VI0 + 0
      {"IAND",   0x800018F4u, 1u}, // VI3 = VI3 & VI0
      {"IOR",    0x800000F5u, 1u}, // VI3 = VI0 | VI0
      {"MTIR",   0x800323FCu, 1u}, // VI3 = low16(VF4.x), initially zero
      {"LQI",    0x81041B7Cu, 0xFFFFu}, // LQI.x VF4,(VI3++) wraps VI3 to zero
      {"SQI",    0x8103237Du, 0xFFFFu}, // SQI.x VF4,(VI3++) wraps VI3 to zero
  }};
  enum class Gap { Adjacent, Nop, VfStall };
  for (const auto& writer : writers) {
    for (const auto gap : {Gap::Adjacent, Gap::Nop, Gap::VfStall}) {
      ps2vita::Memory memory;
      ps2vita::Vu1 vu(memory);
      vu.state().vi[3] = writer.old_value;
      pair(memory, 0u, writer.instruction,
           gap == Gap::VfStall ? add_x(2u, 0u) : kUpperNop);
      const unsigned branch_pc = gap == Gap::Nop ? 16u : 8u;
      if (gap == Gap::Nop) pair(memory, 8u, kLowerNop);
      pair(memory, branch_pc, ibne(3u, 0u, 2u),
           gap == Gap::VfStall ? add_x(3u, 2u) : kUpperNop);
      pair(memory, branch_pc + 8u, kLowerNop);
      vu.start(0u);
      vu.run(gap == Gap::Nop ? 4u : 3u);

      const std::string label = std::string(writer.name) +
          (gap == Gap::Adjacent ? " adjacent branch" :
           gap == Gap::Nop ? " branch after NOP" : " branch after VF stall");
      check(vu.state().vi[3] == 0u, label + " commits the live VI value");
      // Adjacent: old VI3 is nonzero, so branch to 0020. A NOP or three
      // additional VF cycles expires the old value and the branch falls through.
      const unsigned expected_pc = gap == Gap::Adjacent ? 0x20u : branch_pc + 16u;
      check(vu.state().pc == expected_pc, label + " uses the expected VI version");
      check(vu.vf_stall_cycles() == (gap == Gap::VfStall ? 3u : 0u),
            label + " has the independently selected VF stall");
      check(vu.first_unsupported_lower() == 0u &&
                vu.first_unsupported_upper() == 0u,
            label + " executes supported instructions");
    }
  }
}

void test_captured_countdown_boundary() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  vu.enable_store_trace(true);
  vu.state().vi[13] = 1u;
  vu.state().vi[9] = 0x10u;
  vu.state().vf[17] = {{0x11223344u, 0x55667788u, 0x99AABBCCu, 0xFFFF8000u}};
  // Exact pairs from first-vif.bin: subtract, signed branch, delay pair,
  // suppression store, unconditional branch, and its delay pair.
  pair(memory, 0x268u, 0x120D6801u);
  pair(memory, 0x270u, 0x5C006807u);
  pair(memory, 0x278u, kLowerNop, 0x01EEBEAAu);
  pair(memory, 0x280u, 0x03E98800u);
  pair(memory, 0x288u, 0x40000004u);
  pair(memory, 0x290u, kLowerNop);
  pair(memory, 0x2B0u, kLowerNop);
  pair(memory, 0x2B8u, kLowerNop);
  pair(memory, 0x2C0u, kLowerNop);
  vu.start(0x268u);
  vu.run(3u);
  check(vu.state().vi[13] == 0u && vu.state().pc == 0x280u,
        "captured IBLEZ reads pre-decrement VI13=1 and reaches 0280");
  vu.run(3u);
  check(vu.state().pc == 0x2B0u,
        "captured B0288 reaches 02B0 after its 0290 delay pair");
  check(vu.store_records().size() == 4u &&
            vu.store_records()[0].pc == 0x280u &&
            vu.store_records()[3].value == 0xFFFF8000u &&
            memory.vu1_data_word(0x10Cu) == 0xFFFF8000u,
        "captured countdown boundary retains the suppression store at 0280");
}

void test_fmand_branch_exemption() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  vu.state().mac = 0x00D0u;
  vu.state().vi[12] = 0x00D0u;
  vu.state().vi[1] = 0u;
  // Exact clipping lower words. Both FMANDs must make their new VI1 visible
  // to the immediately following IBNE, without integer-writer backup.
  pair(memory, 0x240u, 0x34016000u);
  pair(memory, 0x248u, 0x34010800u);
  pair(memory, 0x250u, 0x52016008u);
  pair(memory, 0x258u, kLowerNop);
  vu.start(0x240u);
  vu.run(4u);
  check(vu.state().vi[1] == 0x00D0u && vu.state().pc == 0x260u,
        "captured FMAND chain exposes current VI1 to IBNE0250");
}

void test_repeated_writes() {
  {
    ps2vita::Memory memory;
    ps2vita::Vu1 vu(memory);
    // 0 -> 1 -> 2. The adjacent same-register chain preserves its first
    // old value (0), so IBNE VI3,VI0 is not taken despite live VI3=2.
    pair(memory, 0u, 0x10031801u);
    pair(memory, 8u, 0x10031801u);
    pair(memory, 16u, ibne(3u, 0u, 2u));
    pair(memory, 24u, kLowerNop);
    vu.start(0u);
    vu.run(4u);
    check(vu.state().vi[3] == 2u && vu.state().pc == 0x20u,
          "adjacent repeated VI writes preserve the value before the chain");
  }
  {
    ps2vita::Memory memory;
    ps2vita::Vu1 vu(memory);
    vu.state().vi[5] = 1u;
    // A NOP ends the first write's window. The second write therefore backs
    // up 1 rather than the earlier 0, and the adjacent comparison sees 1.
    pair(memory, 0u, 0x10031801u);
    pair(memory, 8u, kLowerNop);
    pair(memory, 16u, 0x10031801u);
    pair(memory, 24u, ibne(3u, 5u, 2u));
    pair(memory, 32u, kLowerNop);
    vu.start(0u);
    vu.run(5u);
    check(vu.state().vi[3] == 2u && vu.state().pc == 0x28u,
          "a NOP between repeated writes starts a new old-value window");
  }
}

void test_one_stall_cycle_expires_backup() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  vu.state().vi[3] = 1u;
  pair(memory, 0u, kLowerNop, add_x(2u, 0u));
  pair(memory, 8u, kLowerNop);
  pair(memory, 16u, 0x12031801u); // VI3: 1 -> 0 at cycle 2.
  pair(memory, 24u, ibne(3u, 0u, 2u), add_x(3u, 2u));
  pair(memory, 32u, kLowerNop);
  vu.start(0u);
  vu.run(5u);
  check(vu.vf_stall_cycles() == 1u && vu.state().vi[3] == 0u &&
            vu.state().pc == 0x28u,
        "one extra VF stall cycle expires an adjacent VI backup");
}

void test_ibeq_destination_operand() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  vu.state().vi[3] = 1u;
  pair(memory, 0u, 0x12031801u);
  // IBEQ VI0,VI3,+2 checks the saved value in the It operand, rather than Is.
  pair(memory, 8u, (0x28u << 25) | (3u << 16) | 2u);
  pair(memory, 16u, kLowerNop);
  vu.start(0u);
  vu.run(3u);
  check(vu.state().vi[3] == 0u && vu.state().pc == 0x18u,
        "IBEQ compares the saved It operand rather than the new zero");
}

void test_budget_split_preserves_backup() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  vu.state().vi[3] = 1u;
  pair(memory, 0u, 0x12031801u);
  pair(memory, 8u, ibne(3u, 0u, 2u));
  pair(memory, 16u, kLowerNop);
  vu.start(0u);
  vu.run(1u);
  check(vu.running() && vu.state().pc == 8u && vu.state().vi[3] == 0u,
        "host pair budget leaves a running program after the VI writer");
  vu.run(2u);
  check(vu.state().pc == 0x20u,
        "host budget boundary preserves the old VI for the adjacent branch");
}

void test_end_clears_backup_before_resume() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  // Even a write in the E-bit delay pair is retired by program termination.
  // The adjacent repeated writes save 0 while committing live VI3=2.
  pair(memory, 0u, 0x10031801u, 0x400002FFu);
  pair(memory, 8u, 0x10031801u);
  pair(memory, 16u, ibne(3u, 0u, 2u));
  pair(memory, 24u, kLowerNop);
  vu.start(0u);
  vu.run(2u);
  check(!vu.running() && vu.state().pc == 0x10u && vu.state().vi[3] == 2u,
        "E-bit termination commits the delay-pair VI write");
  vu.resume(); // MSCNT continues from the stopped TPC.
  vu.run(2u);
  check(vu.state().pc == 0x28u,
        "MSCNT branch observes retired VI rather than the ended program's backup");
}

void test_other_register_replaces_backup() {
  ps2vita::Memory memory;
  ps2vita::Vu1 vu(memory);
  vu.state().vi[4] = 1u;
  pair(memory, 0u, 0x10031801u); // VI3: 0 -> 1.
  pair(memory, 8u, 0x12042001u); // VI4: 1 -> 0 replaces the tracked VI3.
  // IBEQ VI3,VI4 sees live VI3=1 and old VI4=1, so it takes the branch.
  pair(memory, 16u, (0x28u << 25) | (4u << 16) | (3u << 11) | 2u);
  pair(memory, 24u, kLowerNop);
  vu.start(0u);
  vu.run(4u);
  check(vu.state().vi[3] == 1u && vu.state().vi[4] == 0u &&
            vu.state().pc == 0x28u,
        "writing another VI selects its old value and the first VI's live value");
}

void test_zero_integer_register_is_immutable() {
  struct Writer {
    const char* name;
    std::uint32_t instruction;
  };
  constexpr std::array<Writer, 9> writers{{
      {"ISUBIU", 0x12001801u}, // VI0 = VI3 - 1
      {"IADDIU", 0x10001801u}, // VI0 = VI3 + 1
      {"IADD",   0x80041830u}, // VI0 = VI3 + VI4
      {"IADDI",  0x80001872u}, // VI0 = VI3 + 1
      {"IAND",   0x80041834u}, // VI0 = VI3 & VI4
      {"IOR",    0x80041835u}, // VI0 = VI3 | VI4
      {"MTIR",   0x800023FCu}, // VI0 = low16(VF4.x)
      {"LQI",    0x8104037Cu}, // LQI.x VF4,(VI0++)
      {"SQI",    0x8100237Du}, // SQI.x VF4,(VI0++)
  }};
  for (const auto& writer : writers) {
    ps2vita::Memory memory;
    ps2vita::Vu1 vu(memory);
    vu.state().vi[3] = 7u;
    vu.state().vi[4] = 3u;
    vu.state().vf[4][0] = 0xDEADBEEFu;
    pair(memory, 0u, writer.instruction);
    vu.start(0u);
    vu.run(1u);
    check(vu.state().vi[0] == 0u && vu.state().vi[3] == 7u &&
              vu.state().vi[4] == 3u && vu.first_unsupported_lower() == 0u,
          std::string(writer.name) + " preserves immutable VI0");
  }
}

} // namespace

int main() {
  test_backed_up_writers();
  test_captured_countdown_boundary();
  test_fmand_branch_exemption();
  test_repeated_writes();
  test_one_stall_cycle_expires_backup();
  test_ibeq_destination_operand();
  test_budget_split_preserves_backup();
  test_end_clears_backup_before_resume();
  test_other_register_replaces_backup();
  test_zero_integer_register_is_immutable();
  if (failures != 0u) {
    std::fprintf(stderr, "%u VU1 branch checks failed\n", failures);
    return 1;
  }
  std::puts("VU1 branch tests passed");
  return 0;
}
