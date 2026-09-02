"""Deterministic profile-guided selection of safe direct AstraIR traces."""

from dataclasses import dataclass
from typing import Dict, FrozenSet, Iterable, List, Mapping, Tuple


@dataclass(frozen=True)
class BlockProfile:
    pc: int
    entries: int
    hard_barrier: bool = False


@dataclass(frozen=True)
class EdgeProfile:
    source: int
    target: int
    transitions: int


@dataclass(frozen=True)
class Trace:
    blocks: Tuple[int, ...]
    closes_loop: bool = False


def form_direct_traces(
    blocks: Iterable[BlockProfile],
    edges: Iterable[EdgeProfile],
    direct_successors: Mapping[int, FrozenSet[int]],
    minimum_probability: float = 0.90,
    maximum_blocks: int = 128,
) -> List[Trace]:
    """Follow uniquely hottest statically direct edges without speculation."""
    if not 0.0 <= minimum_probability <= 1.0:
        raise ValueError("minimum_probability must be between zero and one")
    if maximum_blocks < 1:
        raise ValueError("maximum_blocks must be positive")

    by_pc: Dict[int, BlockProfile] = {block.pc: block for block in blocks}
    outgoing: Dict[int, List[EdgeProfile]] = {}
    for edge in edges:
        if edge.transitions > 0:
            outgoing.setdefault(edge.source, []).append(edge)
    for candidates in outgoing.values():
        candidates.sort(key=lambda edge: (-edge.transitions, edge.target))

    owned = set()
    traces = []
    for entry in sorted(by_pc.values(), key=lambda block: (-block.entries, block.pc)):
        if entry.pc in owned:
            continue
        chain = []
        current = entry.pc
        closes_loop = False
        while len(chain) < maximum_blocks and current in by_pc and current not in owned:
            chain.append(current)
            owned.add(current)
            if by_pc[current].hard_barrier:
                break
            candidates = outgoing.get(current, ())
            if not candidates:
                break
            hottest = candidates[0]
            if len(candidates) > 1 and candidates[1].transitions == hottest.transitions:
                break
            total = sum(edge.transitions for edge in candidates)
            if hottest.transitions / total < minimum_probability:
                break
            if hottest.target not in direct_successors.get(current, frozenset()):
                break
            if hottest.target == chain[0]:
                closes_loop = True
                break
            if hottest.target in owned or hottest.target in chain:
                break
            current = hottest.target
        traces.append(Trace(tuple(chain), closes_loop))
    return traces
