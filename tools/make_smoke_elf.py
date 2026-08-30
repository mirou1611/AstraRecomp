#!/usr/bin/env python3
"""Create a tiny legal-to-redistribute PS2/MIPS ELF for PS2Vita smoke tests."""

import argparse
import struct
from pathlib import Path


def build() -> bytes:
    image = bytearray(0x10C)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH",
        ident, 2, 8, 1, 0x1000, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    program_header = struct.pack(
        "<IIIIIIII", 1, 0x100, 0x1000, 0x1000, 12, 16, 5, 16
    )
    image[: len(header)] = header
    image[52 : 52 + len(program_header)] = program_header
    instructions = (
        0x2402002A,  # addiu v0, zero, 42
        0xAC022000,  # sw    v0, 0x2000(zero)
        0x0000000D,  # break
    )
    image[0x100:0x10C] = struct.pack("<III", *instructions)
    return bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", nargs="?", default="smoke.elf")
    args = parser.parse_args()
    output = Path(args.output)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(build())
    print(f"wrote {len(build())} bytes to {output}")


if __name__ == "__main__":
    main()

