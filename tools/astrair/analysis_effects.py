"""Conservative architectural effects and trace barriers for AstraIR."""

from .ir import Effect, Instruction, Op


_LOAD_EFFECTS = Effect.MEMORY_READ | Effect.MAY_FAULT | Effect.MAY_TOUCH_MMIO
_STORE_EFFECTS = (
    Effect.MEMORY_WRITE
    | Effect.MAY_FAULT
    | Effect.MAY_TOUCH_MMIO
    | Effect.MAY_SCHEDULE_EVENT
)


def effects_for(operation: Op) -> Effect:
    if operation in (Op.LW, Op.LD):
        return _LOAD_EFFECTS
    if operation in (Op.SW, Op.SD):
        return _STORE_EFFECTS
    return Effect.PURE


def is_hard_barrier(instruction: Instruction) -> bool:
    """Return whether a future multi-block trace must stop at this operation."""
    return bool(
        instruction.effects
        & (Effect.MAY_FAULT | Effect.MAY_TOUCH_MMIO | Effect.MAY_SCHEDULE_EVENT)
    )
