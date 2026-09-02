"""Conservative scalar width facts for the initial AstraIR subset."""

from dataclasses import replace
from typing import Iterable, List, Tuple

from .ir import Instruction, Op, UpperBits, ValueKind


def result_for(operation: Op, rt: int, rd: int) -> Tuple[int, ValueKind, UpperBits]:
    if operation in (Op.ADDIU, Op.LUI, Op.LW):
        return rt, ValueKind.S32, UpperBits.SIGN_EXTENDED_32
    if operation in (Op.ADDU, Op.MULT):
        return rd, ValueKind.S32, UpperBits.SIGN_EXTENDED_32
    if operation is Op.ANDI:
        return rt, ValueKind.U16, UpperBits.ZERO
    if operation is Op.SLTU:
        return rd, ValueKind.I1, UpperBits.ZERO
    if operation in (Op.ORI, Op.LD):
        return rt, ValueKind.U64, UpperBits.UNKNOWN
    if operation in (Op.AND, Op.DADDU, Op.DSUBU):
        return rd, ValueKind.U64, UpperBits.UNKNOWN
    if operation in (Op.PAND, Op.PXOR, Op.POR):
        return rd, ValueKind.V128, UpperBits.UNKNOWN
    return 0, ValueKind.NONE, UpperBits.UNKNOWN


def infer_block_results(instructions: Iterable[Instruction]) -> List[Instruction]:
    """Propagate safe low-width facts through a straight-line IR sequence."""
    facts = [(ValueKind.U64, UpperBits.UNKNOWN) for _ in range(32)]
    facts[0] = (ValueKind.I1, UpperBits.ZERO)
    result = []
    for instruction in instructions:
        kind = instruction.result_kind
        upper = instruction.upper_bits
        if instruction.op is Op.ORI:
            source_kind, source_upper = facts[instruction.rs]
            if source_upper is UpperBits.SIGN_EXTENDED_32:
                kind, upper = ValueKind.S32, UpperBits.SIGN_EXTENDED_32
            elif source_upper is UpperBits.ZERO and source_kind in (
                ValueKind.I1, ValueKind.U16
            ):
                kind, upper = ValueKind.U16, UpperBits.ZERO
        elif instruction.op is Op.AND:
            left = facts[instruction.rs]
            right = facts[instruction.rt]
            zero_widths = [
                fact[0] for fact in (left, right)
                if fact[1] is UpperBits.ZERO
                and fact[0] in (ValueKind.I1, ValueKind.U16)
            ]
            if zero_widths:
                kind = (ValueKind.I1 if left[0] is ValueKind.I1
                        and right[0] is ValueKind.I1 else ValueKind.U16)
                upper = UpperBits.ZERO

        refined = replace(instruction, result_kind=kind, upper_bits=upper)
        result.append(refined)
        if refined.result_register:
            facts[refined.result_register] = (kind, upper)
    return result
