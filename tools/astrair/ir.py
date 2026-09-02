"""Semantic instruction records for AstraRecomp's initial AOT subset."""

from dataclasses import dataclass
from enum import Enum, auto


class Op(Enum):
    NOP = auto()
    ADDIU = auto()
    ADDU = auto()
    AND = auto()
    SLTU = auto()
    DADDU = auto()
    DSUBU = auto()
    MULT = auto()
    PAND = auto()
    PXOR = auto()
    POR = auto()
    ANDI = auto()
    ORI = auto()
    LUI = auto()
    LW = auto()
    SW = auto()
    LD = auto()
    SD = auto()


@dataclass(frozen=True)
class Instruction:
    pc: int
    word: int
    op: Op
    rs: int = 0
    rt: int = 0
    rd: int = 0
    immediate: int = 0
