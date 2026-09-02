"""Decode the supported EE instruction subset into minimal AstraIR."""

from typing import Optional

from .analysis_effects import effects_for
from .analysis_width import result_for
from .ir import Instruction, Op, UpperBits, ValueKind


def _sx16(word: int) -> int:
    value = word & 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def build_data_instruction(pc: int, word: int) -> Optional[Instruction]:
    primary = word >> 26
    function = word & 0x3F
    sub = (word >> 6) & 0x1F
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31

    kind = None
    if word == 0:
        kind = Op.NOP
    elif primary == 0x09:
        kind = Op.ADDIU
    elif primary == 0x00:
        kind = {
            0x18: Op.MULT,
            0x21: Op.ADDU,
            0x24: Op.AND,
            0x2B: Op.SLTU,
            0x2D: Op.DADDU,
            0x2F: Op.DSUBU,
        }.get(function)
    elif primary == 0x1C:
        kind = {
            (0x09, 0x12): Op.PAND,
            (0x09, 0x13): Op.PXOR,
            (0x29, 0x12): Op.POR,
        }.get((function, sub))
    else:
        kind = {
            0x0C: Op.ANDI,
            0x0D: Op.ORI,
            0x0F: Op.LUI,
            0x23: Op.LW,
            0x2B: Op.SW,
            0x37: Op.LD,
            0x3F: Op.SD,
        }.get(primary)

    if kind is None:
        return None
    result_register, result_kind, upper_bits = result_for(kind, rt, rd)
    if result_register == 0:
        result_kind = ValueKind.NONE
        upper_bits = UpperBits.UNKNOWN
    return Instruction(
        pc=pc,
        word=word,
        op=kind,
        rs=rs,
        rt=rt,
        rd=rd,
        immediate=_sx16(word),
        effects=effects_for(kind),
        result_register=result_register,
        result_kind=result_kind,
        upper_bits=upper_bits,
    )
