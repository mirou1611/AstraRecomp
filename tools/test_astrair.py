#!/usr/bin/env python3
"""Contract tests for the minimal AstraIR instruction builder."""

import unittest

from astrair.builder import build_data_instruction
from astrair.analysis_effects import is_hard_barrier
from astrair.analysis_liveness import analyze_block
from astrair.analysis_width import infer_block_results
from astrair.ir import Effect, Op, UpperBits, ValueKind
from generate_astrart import defer_gpr_writeback


def r_type(rs: int, rt: int, rd: int, sub: int, function: int) -> int:
    return (rs << 21) | (rt << 16) | (rd << 11) | (sub << 6) | function


def i_type(primary: int, rs: int, rt: int, immediate: int) -> int:
    return (primary << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


class AstraIrBuilderTests(unittest.TestCase):
    def test_supported_subset_has_typed_operations(self) -> None:
        cases = {
            0: Op.NOP,
            i_type(0x09, 3, 2, -4): Op.ADDIU,
            r_type(3, 4, 2, 0, 0x21): Op.ADDU,
            r_type(3, 4, 2, 0, 0x24): Op.AND,
            r_type(3, 4, 2, 0, 0x2B): Op.SLTU,
            r_type(3, 4, 2, 0, 0x2D): Op.DADDU,
            r_type(3, 4, 2, 0, 0x2F): Op.DSUBU,
            r_type(3, 4, 2, 0, 0x18): Op.MULT,
            r_type(3, 4, 2, 0x12, 0x09) | (0x1C << 26): Op.PAND,
            r_type(3, 4, 2, 0x13, 0x09) | (0x1C << 26): Op.PXOR,
            r_type(3, 4, 2, 0x12, 0x29) | (0x1C << 26): Op.POR,
            i_type(0x0C, 3, 2, 0x1234): Op.ANDI,
            i_type(0x0D, 3, 2, 0x1234): Op.ORI,
            i_type(0x0F, 0, 2, 0x1234): Op.LUI,
            i_type(0x23, 3, 2, -4): Op.LW,
            i_type(0x2B, 3, 2, -4): Op.SW,
            i_type(0x37, 3, 2, -8): Op.LD,
            i_type(0x3F, 3, 2, -8): Op.SD,
        }
        for word, expected in cases.items():
            with self.subTest(word=f"0x{word:08x}"):
                instruction = build_data_instruction(0x1000, word)
                self.assertIsNotNone(instruction)
                self.assertEqual(instruction.op, expected)
                self.assertEqual(instruction.pc, 0x1000)
                self.assertEqual(instruction.word, word)

    def test_fields_and_signed_immediate_are_preserved(self) -> None:
        instruction = build_data_instruction(0x2000, i_type(0x09, 7, 9, -4))
        self.assertEqual(
            (instruction.rs, instruction.rt, instruction.rd,
             instruction.immediate),
            (7, 9, 31, -4),
        )

    def test_control_and_unknown_operations_are_not_data_ir(self) -> None:
        self.assertIsNone(build_data_instruction(0x1000, i_type(0x04, 1, 2, 1)))
        self.assertIsNone(build_data_instruction(0x1000, i_type(0x08, 1, 2, -1)))

    def test_effects_are_conservative_trace_barriers(self) -> None:
        pure = build_data_instruction(0x1000, r_type(3, 4, 2, 0, 0x21))
        load = build_data_instruction(0x1004, i_type(0x23, 3, 2, 4))
        store = build_data_instruction(0x1008, i_type(0x2B, 3, 2, 4))
        self.assertEqual(pure.effects, Effect.PURE)
        self.assertFalse(is_hard_barrier(pure))
        self.assertEqual(
            load.effects,
            Effect.MEMORY_READ | Effect.MAY_FAULT | Effect.MAY_TOUCH_MMIO,
        )
        self.assertTrue(is_hard_barrier(load))
        self.assertEqual(
            store.effects,
            Effect.MEMORY_WRITE
            | Effect.MAY_FAULT
            | Effect.MAY_TOUCH_MMIO
            | Effect.MAY_SCHEDULE_EVENT,
        )
        self.assertTrue(is_hard_barrier(store))

    def test_static_result_widths_and_upper_bits(self) -> None:
        cases = (
            (i_type(0x09, 3, 2, -4), 2, ValueKind.S32,
             UpperBits.SIGN_EXTENDED_32),
            (i_type(0x0C, 3, 2, 0x1234), 2, ValueKind.U16,
             UpperBits.ZERO),
            (r_type(3, 4, 2, 0, 0x2B), 2, ValueKind.I1,
             UpperBits.ZERO),
            (i_type(0x37, 3, 2, -8), 2, ValueKind.U64,
             UpperBits.UNKNOWN),
            (r_type(3, 4, 2, 0x12, 0x09) | (0x1C << 26), 2,
             ValueKind.V128, UpperBits.UNKNOWN),
            (i_type(0x2B, 3, 2, 4), 0, ValueKind.NONE,
             UpperBits.UNKNOWN),
        )
        for word, register, kind, upper in cases:
            with self.subTest(word=f"0x{word:08x}"):
                instruction = build_data_instruction(0x1000, word)
                self.assertEqual(instruction.result_register, register)
                self.assertEqual(instruction.result_kind, kind)
                self.assertEqual(instruction.upper_bits, upper)

    def test_write_to_zero_has_no_result(self) -> None:
        instruction = build_data_instruction(0x1000, i_type(0x09, 3, 0, 1))
        self.assertEqual(instruction.result_register, 0)
        self.assertEqual(instruction.result_kind, ValueKind.NONE)

    def test_block_width_inference_propagates_safe_facts(self) -> None:
        words = (
            i_type(0x0F, 0, 2, 0x1234),
            i_type(0x0D, 2, 3, 0x5678),
            i_type(0x0D, 0, 4, 0x00FF),
            r_type(3, 4, 5, 0, 0x24),
        )
        instructions = [
            build_data_instruction(0x1000 + index * 4, word)
            for index, word in enumerate(words)
        ]
        inferred = infer_block_results(instructions)
        self.assertEqual(
            (inferred[1].result_kind, inferred[1].upper_bits),
            (ValueKind.S32, UpperBits.SIGN_EXTENDED_32),
        )
        self.assertEqual(
            (inferred[2].result_kind, inferred[2].upper_bits),
            (ValueKind.U16, UpperBits.ZERO),
        )
        self.assertEqual(
            (inferred[3].result_kind, inferred[3].upper_bits),
            (ValueKind.U16, UpperBits.ZERO),
        )

    def test_register_use_def_and_backward_liveness(self) -> None:
        instructions = [
            build_data_instruction(0x1000, i_type(0x09, 4, 2, 1)),
            build_data_instruction(0x1004, i_type(0x09, 5, 2, 2)),
            build_data_instruction(0x1008, r_type(2, 6, 3, 0, 0x21)),
        ]
        live = analyze_block(instructions, live_out={3})
        self.assertEqual(instructions[0].reads, frozenset({4}))
        self.assertEqual(instructions[0].writes, frozenset({2}))
        self.assertEqual(live[0].dead_writes, frozenset({2}))
        self.assertEqual(live[1].dead_writes, frozenset())
        self.assertEqual(live[2].live_out, frozenset({3}))
        self.assertEqual(live[0].live_in, frozenset({4, 5, 6}))

    def test_store_reads_address_and_value_without_writing_gpr(self) -> None:
        store = build_data_instruction(0x1000, i_type(0x2B, 3, 2, 4))
        self.assertEqual(store.reads, frozenset({2, 3}))
        self.assertEqual(store.writes, frozenset())

    def test_deferred_writeback_flushes_every_commit_path(self) -> None:
        source = [
            "AotExit generated(Memory& memory, CpuState& state) {",
            "  std::uint32_t executed = 0;",
            "  std::uint32_t fast = 0;",
            "  state.gpr[2] = state.gpr[4] + state.gpr[0];",
            "  if (fault) {",
            "    commit(memory, state, executed, fast);",
            "    return {};",
            "  }",
            "  commit(memory, state, executed, fast);",
            "  return {};",
            "}",
        ]
        generated = defer_gpr_writeback(source)
        self.assertIn("  std::uint64_t gpr_2 = state.gpr[2];", generated)
        self.assertIn("  std::uint64_t gpr_4 = state.gpr[4];", generated)
        self.assertIn("  gpr_2 = gpr_4 + state.gpr[0];", generated)
        self.assertEqual(generated.count("    state.gpr[2] = gpr_2;"), 1)
        self.assertEqual(generated.count("  state.gpr[2] = gpr_2;"), 1)
        for index, line in enumerate(generated):
            if "commit(memory, state, executed, fast);" in line:
                self.assertIn("state.gpr[2] = gpr_2;", generated[index - 1])


if __name__ == "__main__":
    unittest.main()
