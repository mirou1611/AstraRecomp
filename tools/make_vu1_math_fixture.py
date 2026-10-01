#!/usr/bin/env python3
"""Generate an owned, qword-aligned VIF1 finite DIV/MUL observation packet.

Operands are raw IEEE words, uploaded to VU1 qwords 0x100/0x101. Each case
uploads and runs its own short microprogram at address zero, then FLUSHE waits
for normal E-bit termination. Outputs occupy qwords 0x000 through 0x00F.
DIV writes Q through MULq.w vf3,vf0.w; vector MUL writes all four lanes.

No BIOS microprogram or emulator implementation is copied. The four TriAce
expected values are externally recorded VU0 MULi data transposed to VU1 MUL,
not hardware validation of this instruction form, unit, or surrounding code.
Other cases are observations and deliberately have no native-float oracle.
"""

import argparse
import hashlib
import json
import struct
from dataclasses import dataclass
from pathlib import Path


LOWER_NOP = 0x8000033C
UPPER_NOP = 0x000002FF
END_BIT = 0x40000000
INPUT_QWORDS = (0x100, 0x101)
TRIACE_REVISION = "97469ffbed8631277b94e28d01dabd702aa97ef3"
TRIACE_SOURCE = (
    "https://github.com/unknownbrackets/ps2autotests/blob/"
    f"{TRIACE_REVISION}/tests/vu/games/triace.expected"
)
HARDWARE_SCOPE = (
    "Recorded VU0 MULi operands/results transposed to VU1 vector MUL with "
    "identical lanes; not full hardware equivalence. The pinned Reliquary "
    "DoMul adapter separately corroborated these four arithmetic results."
)


@dataclass(frozen=True)
class MathCase:
    name: str
    operation: str
    lhs: int
    rhs: int
    recorded_vu0_muli: int | None = None


def cases() -> tuple[MathCase, ...]:
    result = []
    for label, denominator in (("2p5", 0x40200000), ("3", 0x40400000)):
        for sign_label, lhs_sign, rhs_sign in (
            ("pp", 0, 0), ("pn", 0, 0x80000000),
            ("np", 0x80000000, 0), ("nn", 0x80000000, 0x80000000),
        ):
            result.append(MathCase(f"div_1_{label}_{sign_label}", "div",
                                   0x3F800000 | lhs_sign, denominator | rhs_sign))
    result.extend((
        MathCase("div_1_2_exact", "div", 0x3F800000, 0x40000000),
        MathCase("div_neg1_2_exact", "div", 0xBF800000, 0x40000000),
        MathCase("mul_triace_1", "mul", 0xB063B75B, 0x42FECCCD, 0xB3E2A618),
        MathCase("mul_triace_2", "mul", 0x8701CE82, 0x43D80D3E, 0x8B5B19E9),
        MathCase("mul_triace_3", "mul", 0x46FCC888, 0x43A0DA10, 0x4B1ED4A7),
        MathCase("mul_triace_4", "mul", 0x793CC535, 0x43546E14, 0x7D1CA47B),
        MathCase("mul_0p01_0p4_cc", "mul", 0x3C23D70A, 0x3ECCCCCC),
        MathCase("mul_0p01_0p4_cd", "mul", 0x3C23D70A, 0x3ECCCCCD),
    ))
    return tuple(result)


def microprogram(operation: str, output_qword: int) -> tuple[tuple[int, int], ...]:
    """Hand-encode loads, arithmetic, a store, E/NOP and its delay pair.

    Sources are not read until four pairs after the last LQ. SQ likewise reads
    VF3 only four pairs after its arithmetic writer. WAITQ is a separate pair
    from MULq so the consumer cannot read Q before WAITQ has retired it.
    ACC is unused by these instruction forms. VF3 is explicitly cleared.
    """
    if operation not in ("div", "mul"):
        raise ValueError("operation must be div or mul")
    if not 0 <= output_qword <= 0xF:
        raise ValueError("output qword must be in 0x000..0x00F")
    program = [(LOWER_NOP, UPPER_NOP) for _ in range(13)]
    clear_vf3 = (15 << 21) | (3 << 6) | 0x2C  # SUB.xyzw vf3,vf0,vf0
    program[0] = ((15 << 21) | (1 << 16) | INPUT_QWORDS[0], clear_vf3)
    program[1] = ((15 << 21) | (2 << 16) | INPUT_QWORDS[1], UPPER_NOP)
    if operation == "div":
        program[5] = (0x800003BC | (2 << 16) | (1 << 11), UPPER_NOP)
        program[6] = (0x800003BF, UPPER_NOP)  # WAITQ
        program[7] = (LOWER_NOP, (1 << 21) | (3 << 6) | 0x1C)  # MULq.w vf3,vf0
        store_pair = 11
    else:
        program[5] = (LOWER_NOP,
                      (15 << 21) | (2 << 16) | (1 << 11) | (3 << 6) | 0x2A)
        store_pair = 9
    store = (1 << 25) | (15 << 21) | (3 << 11) | output_qword
    program[store_pair] = (store, UPPER_NOP)
    lower, upper = program[11]
    program[11] = (lower, upper | END_BIT)
    return tuple(program)


def build() -> tuple[bytes, dict]:
    """Return a packet and descriptive metadata without calculating float results."""
    # STCYCL WL=CL=1 and STMOD normal make UNPACK independent of prior state.
    words = [0x10000000, 0x01000101, 0x05000000, 0]
    layout = []
    for index, case in enumerate(cases()):
        case_start = len(words) * 4
        # Put the V4-32 payload at a qword boundary. FLG=0 uses absolute addresses.
        words.extend((0, 0, 0, 0x6C020100))
        operand_offset = len(words) * 4
        words.extend((case.lhs,) * 4 + (case.rhs,) * 4)
        program = microprogram(case.operation, index)
        # MPG payload is eight-byte aligned; its count is in instruction pairs.
        words.append(0)
        mpg_offset = len(words) * 4
        words.append(0x4A000000 | (len(program) << 16))
        micro_offset = len(words) * 4
        for lower, upper in program:
            words.extend((lower, upper))
        launch_offset = len(words) * 4
        words.extend((0x14000000, 0x10000000, 0, 0))  # MSCAL 0, FLUSHE, NOPs
        entry = {
            "name": case.name,
            "operation": case.operation,
            "lhs": f"{case.lhs:08X}",
            "rhs": f"{case.rhs:08X}",
            "input_qwords": [f"{address:03X}" for address in INPUT_QWORDS],
            "input_payload_offset": operand_offset,
            "case_start_offset": case_start,
            "mpg_command_offset": mpg_offset,
            "micro_payload_offset": micro_offset,
            "micro_pairs": len(program),
            "mscal_command_offset": launch_offset,
            "output_qword": f"{index:03X}",
            "output_byte_offset": index * 16,
            "result_lanes": "w" if case.operation == "div" else "xyzw",
            "other_lanes": "explicitly cleared to zero" if case.operation == "div" else None,
        }
        if case.recorded_vu0_muli is not None:
            entry["recorded_vu0_muli"] = f"{case.recorded_vu0_muli:08X}"
            entry["recorded_source"] = TRIACE_SOURCE
            entry["recorded_scope"] = HARDWARE_SCOPE
        layout.append(entry)
    packet = struct.pack(f"<{len(words)}I", *words)
    metadata = {
        "schema_version": 1,
        "packet_bytes": len(packet),
        "packet_sha256": hashlib.sha256(packet).hexdigest(),
        "scope": "Owned finite arithmetic observation stream; no native-float expected values.",
        "hardware_scope": HARDWARE_SCOPE,
        "cases": layout,
    }
    return packet, metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packet", type=Path, help="output qword-aligned VIF binary")
    parser.add_argument("--layout", "--json", dest="layout", type=Path,
                        help="optional JSON raw operands, output locations and pinned evidence")
    args = parser.parse_args()
    if args.layout is not None and args.packet.resolve() == args.layout.resolve():
        parser.error("packet and JSON layout must use distinct paths")
    packet, metadata = build()
    args.packet.parent.mkdir(parents=True, exist_ok=True)
    args.packet.write_bytes(packet)
    if args.layout is not None:
        args.layout.parent.mkdir(parents=True, exist_ok=True)
        args.layout.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {len(packet)} bytes, {len(metadata['cases'])} cases to {args.packet}; "
          f"sha256={metadata['packet_sha256']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
