"""Deterministic profile-guided selection of safe direct AstraIR traces."""

from dataclasses import dataclass
import json
from typing import Dict, FrozenSet, Iterable, List, Mapping, Tuple

from astrair.deopt import DeoptMap, Guard, validate_deopt_metadata


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
    guards: Tuple[Guard, ...] = ()


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
            static_targets = direct_successors.get(current, frozenset())
            # A conditional branch can be highly biased, but following it still
            # needs a guard and deopt route. This selector is guard-free only.
            if len(static_targets) != 1 or hottest.target not in static_targets:
                break
            if hottest.target == chain[0]:
                closes_loop = True
                break
            if hottest.target in owned or hottest.target in chain:
                break
            current = hottest.target
        traces.append(Trace(tuple(chain), closes_loop))
    return traces


def serialize_trace_plan(traces: Iterable[Trace], source_fingerprint: str,
                         deopt_maps: Iterable[DeoptMap] = ()) -> str:
    """Return a stable, reviewable plan without changing emitted code."""
    traces = tuple(traces)
    deopt_maps = tuple(deopt_maps)
    guards = tuple(guard for trace in traces for guard in trace.guards)
    validate_deopt_metadata(guards, deopt_maps)
    document = {
        "schema": "astrarecomp.trace-plan",
        "version": 1,
        "source": {
            "fingerprint_scheme": "sha256-length-prefixed-elf-set-v1",
            "fingerprint_sha256": source_fingerprint,
        },
        "traces": [
            {
                "trace_id": index,
                "blocks": [f"0x{pc:08X}" for pc in trace.blocks],
                "closes_loop": trace.closes_loop,
                "guards": [
                    {
                        "kind": guard.kind.value,
                        "guest_pc": f"0x{guard.guest_pc:08X}",
                        "expected": guard.expected,
                        "side_exit_pc": f"0x{guard.side_exit_pc:08X}",
                        "deopt_id": guard.deopt_id,
                    }
                    for guard in trace.guards
                ],
            }
            for index, trace in enumerate(traces)
        ],
        "deopt_maps": [
            {
                "deopt_id": deopt_map.deopt_id,
                "resume_pc": f"0x{deopt_map.resume_pc:08X}",
                "recoveries": [
                    {
                        "register": recovery.register,
                        "high_half": recovery.high_half,
                        "kind": recovery.kind.value,
                        "value": recovery.value,
                    }
                    for recovery in deopt_map.recoveries
                ],
            }
            for deopt_map in sorted(deopt_maps, key=lambda item: item.deopt_id)
        ],
    }
    return json.dumps(document, indent=2, sort_keys=True) + "\n"
