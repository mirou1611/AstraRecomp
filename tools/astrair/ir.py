"""Semantic instruction records for AstraRecomp's initial AOT subset."""

from dataclasses import dataclass, field
from enum import Enum, IntFlag, auto


class Effect(IntFlag):
    PURE = 0
    MEMORY_READ = auto()
    MEMORY_WRITE = auto()
    MAY_FAULT = auto()
    MAY_TOUCH_MMIO = auto()
    MAY_SCHEDULE_EVENT = auto()


class ValueKind(Enum):
    NONE = auto()
    I1 = auto()
    U16 = auto()
    S32 = auto()
    U64 = auto()
    V128 = auto()


class UpperBits(Enum):
    UNKNOWN = auto()
    ZERO = auto()
    SIGN_EXTENDED_32 = auto()


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
    effects: Effect = Effect.PURE
    result_register: int = 0
    result_kind: ValueKind = ValueKind.NONE
    upper_bits: UpperBits = UpperBits.UNKNOWN
    reads: frozenset = field(default_factory=frozenset)
    writes: frozenset = field(default_factory=frozenset)
