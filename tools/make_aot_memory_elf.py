#!/usr/bin/env python3
"""Create AstraRT backend corpora for memory semantics and fallback tests."""

import argparse
import struct
from pathlib import Path


def elf(entry: int, instructions: tuple[int, ...]) -> bytes:
    code = struct.pack(f"<{len(instructions)}I", *instructions)
    image = bytearray(0x100 + len(code))
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    image[:52] = struct.pack(
        "<16sHHIIIIIHHHHHH",
        ident, 2, 8, 1, entry, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    image[52:84] = struct.pack(
        "<IIIIIIII", 1, 0x100, entry, entry, len(code), len(code), 5, 16
    )
    image[0x100:] = code
    return bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument("--unsupported", action="store_true")
    parser.add_argument("--branches", action="store_true")
    parser.add_argument("--branches-bne", action="store_true")
    args = parser.parse_args()
    if args.branches_bne:
        payload = elf(0x7000, (
            0x14850003,  # bne   a0, a1, 0x7010
            0x24020001,  # addiu v0, zero, 1 (delay slot)
            0x08001C06,  # j     0x7018
            0x24420002,  # addiu v0, v0, 2 (delay slot)
            0x08001C06,  # j     0x7018
            0x24420004,  # addiu v0, v0, 4 (delay slot)
            0x0000000D,  # break
        ))
    elif args.branches:
        payload = elf(0x6000, (
            0x10850003,  # beq   a0, a1, 0x6010
            0x24020001,  # addiu v0, zero, 1 (delay slot)
            0x08001806,  # j     0x6018
            0x24420002,  # addiu v0, v0, 2 (delay slot)
            0x08001806,  # j     0x6018
            0x24420004,  # addiu v0, v0, 4 (delay slot)
            0x0000000D,  # break
        ))
    elif args.unsupported:
        payload = elf(0x5000, (0x34020001, 0x0000000D))  # ori: fallback boundary
    else:
        payload = elf(0x4000, (
            0x00851021,  # addu  v0, a0, a1
            0xACC20000,  # sw    v0, 0(a2)
            0x8CC30000,  # lw    v1, 0(a2)
            0x2462FFF9,  # addiu v0, v1, -7
            0x0000000D,  # break
        ))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    print(f"wrote {len(payload)} bytes to {args.output}")


if __name__ == "__main__":
    main()
