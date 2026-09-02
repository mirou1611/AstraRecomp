#!/usr/bin/env python3
"""Contract tests for the minimal AstraIR instruction builder."""

import unittest

from astrair.builder import build_data_instruction
from astrair.ir import Op


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


if __name__ == "__main__":
    unittest.main()
