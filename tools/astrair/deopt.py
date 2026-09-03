"""Backend-neutral guard and deoptimization metadata contracts."""

from dataclasses import dataclass
from enum import Enum
from typing import Iterable, Tuple


class GuardKind(str, Enum):
    BRANCH_DIRECTION = "branch_direction"


class RecoveryKind(str, Enum):
    ARCHITECTURAL = "architectural"
    TRACE_LOCAL = "trace_local"
    CONSTANT = "constant"


@dataclass(frozen=True)
class RegisterRecovery:
    register: int
    high_half: bool
    kind: RecoveryKind
    value: int = 0


@dataclass(frozen=True)
class DeoptMap:
    deopt_id: int
    resume_pc: int
    recoveries: Tuple[RegisterRecovery, ...] = ()


@dataclass(frozen=True)
class Guard:
    kind: GuardKind
    guest_pc: int
    expected: int
    side_exit_pc: int
    deopt_id: int


def validate_deopt_metadata(guards: Iterable[Guard],
                            deopt_maps: Iterable[DeoptMap]) -> None:
    """Reject incomplete or ambiguous state-reconstruction metadata."""
    maps = tuple(deopt_maps)
    by_id = {}
    for deopt_map in maps:
        if deopt_map.deopt_id < 0:
            raise ValueError("deopt IDs must be non-negative")
        if deopt_map.deopt_id in by_id:
            raise ValueError(f"duplicate deopt ID {deopt_map.deopt_id}")
        if deopt_map.resume_pc < 0 or deopt_map.resume_pc > 0xFFFFFFFF:
            raise ValueError("deopt resume PC must fit in 32 bits")
        if deopt_map.resume_pc & 3:
            raise ValueError("deopt resume PC must be instruction-aligned")
        recovered = set()
        for recovery in deopt_map.recoveries:
            if not isinstance(recovery.kind, RecoveryKind):
                raise ValueError("unknown deopt recovery kind")
            if recovery.register <= 0 or recovery.register >= 32:
                raise ValueError("deopt recovery register must be between 1 and 31")
            key = (recovery.register, recovery.high_half)
            if key in recovered:
                raise ValueError("duplicate register recovery in deopt map")
            recovered.add(key)
            if recovery.value < 0 or recovery.value > 0xFFFFFFFFFFFFFFFF:
                raise ValueError("deopt recovery value must fit in 64 bits")
            if recovery.kind is RecoveryKind.ARCHITECTURAL and recovery.value != 0:
                raise ValueError("architectural recovery must not name a value")
        by_id[deopt_map.deopt_id] = deopt_map

    for guard in guards:
        if not isinstance(guard.kind, GuardKind):
            raise ValueError("unknown guard kind")
        if guard.deopt_id not in by_id:
            raise ValueError(f"guard references missing deopt ID {guard.deopt_id}")
        if guard.guest_pc < 0 or guard.guest_pc > 0xFFFFFFFF or guard.guest_pc & 3:
            raise ValueError("guard PC must be an aligned 32-bit address")
        if (guard.side_exit_pc < 0 or guard.side_exit_pc > 0xFFFFFFFF or
                guard.side_exit_pc & 3):
            raise ValueError("guard side-exit PC must be an aligned 32-bit address")
        if guard.kind is GuardKind.BRANCH_DIRECTION and guard.expected not in (0, 1):
            raise ValueError("branch-direction guard expectation must be zero or one")
        if by_id[guard.deopt_id].resume_pc != guard.side_exit_pc:
            raise ValueError("guard side exit must match its deopt resume PC")
