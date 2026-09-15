"""GPR use/def classification and backward liveness for AstraIR blocks."""

from dataclasses import dataclass
from typing import FrozenSet, Iterable, List, Set, Tuple

from .ir import Instruction, Op


_RS_RT_RD = {
    Op.ADDU, Op.AND, Op.SLTU, Op.DADDU, Op.DSUBU, Op.MULT,
    Op.PAND, Op.PXOR, Op.POR,
}
_RS_RT = {Op.ADDIU, Op.ANDI, Op.ORI, Op.LW, Op.LD}
_STORE = {Op.SW, Op.SD}


def registers_for(operation: Op, rs: int, rt: int, rd: int
                  ) -> Tuple[FrozenSet[int], FrozenSet[int]]:
    reads: Set[int] = set()
    writes: Set[int] = set()
    if operation in _RS_RT_RD:
        reads.update((rs, rt))
        writes.add(rd)
    elif operation in _RS_RT:
        reads.add(rs)
        writes.add(rt)
    elif operation is Op.LUI:
        writes.add(rt)
    elif operation in _STORE:
        reads.update((rs, rt))
    reads.discard(0)
    writes.discard(0)
    return frozenset(reads), frozenset(writes)


@dataclass(frozen=True)
class LiveInstruction:
    instruction: Instruction
    live_in: FrozenSet[int]
    live_out: FrozenSet[int]
    dead_writes: FrozenSet[int]


def analyze_block(instructions: Iterable[Instruction],
                  live_out: Iterable[int] = ()) -> List[LiveInstruction]:
    sequence = list(instructions)
    live = set(live_out)
    reversed_result = []
    for instruction in reversed(sequence):
        outgoing = frozenset(live)
        dead = instruction.writes - outgoing
        live.difference_update(instruction.writes)
        live.update(instruction.reads)
        reversed_result.append(LiveInstruction(
            instruction=instruction,
            live_in=frozenset(live),
            live_out=outgoing,
            dead_writes=frozenset(dead),
        ))
    return list(reversed(reversed_result))
