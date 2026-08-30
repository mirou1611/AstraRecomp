#!/usr/bin/env python3
"""Create a redistributable two-function PS2 ELF for AOT chaining tests."""

import argparse
import struct
from pathlib import Path


def build() -> bytes:
    # main at 0x3000 calls leaf at 0x3020. The JAL delay slot initializes a0;
    # the returned value is incremented and stored before BREAK.
    instructions = (
        0x0C000C08,  # 3000: jal   0x3020
        0x24040028,  # 3004: addiu a0, zero, 40 (delay slot)
        0x24420002,  # 3008: addiu v0, v0, 2
        0xAC022004,  # 300c: sw    v0, 0x2004(zero)
        0x0000000D,  # 3010: break
        0x00000000,  # 3014: padding
        0x00000000,  # 3018: padding
        0x00000000,  # 301c: padding
        0x00801021,  # 3020: addu  v0, a0, zero
        0x03E00008,  # 3024: jr    ra
        0x00000000,  # 3028: nop (delay slot)
    )
    code = struct.pack(f"<{len(instructions)}I", *instructions)
    image = bytearray(0x100 + len(code))
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    header = struct.pack(
        "<16sHHIIIIIHHHHHH",
        ident, 2, 8, 1, 0x3000, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    program_header = struct.pack(
        "<IIIIIIII", 1, 0x100, 0x3000, 0x3000, len(code), len(code), 5, 16
    )
    image[: len(header)] = header
    image[52 : 52 + len(program_header)] = program_header
    image[0x100:] = code
    return bytes(image)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", nargs="?", default="aot-chain.elf")
    output = Path(parser.parse_args().output)
    output.parent.mkdir(parents=True, exist_ok=True)
    payload = build()
    output.write_bytes(payload)
    print(f"wrote {len(payload)} bytes to {output}")


if __name__ == "__main__":
    main()
