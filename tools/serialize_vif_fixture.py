#!/usr/bin/env python3
"""Make a controlled synchronous-functional VIF fixture, not a BIOS timing oracle.

Insert FLUSHE and three NOP words after each command-boundary MSCAL, MSCALF,
or MSCNT. This deliberately serializes VU execution before subsequent uploads.
It changes the event ordering of an asynchronous reference; it does not recover
the original BIOS DMA arrival times, EE writes, or GIF arbitration.

Every input byte is retained in order. Each insertion is 16 bytes, so original
command and payload offsets retain their modulo-16 alignment. No padding is
added, removed, or guessed inside payloads. Only the complete, known Astra VIF
subset below is accepted; upload and DIRECT data are never scanned for opcodes.
The input is a self-contained word stream with reset STCYCL state (CL=WL=0).
"""

import argparse
import hashlib
import json
import struct
from pathlib import Path


BARRIER = struct.pack("<4I", 0x10000000, 0, 0, 0)
START_COMMANDS = {0x14: "MSCAL", 0x15: "MSCALF", 0x17: "MSCNT"}
HEADER_COMMANDS = frozenset((0x00, *range(0x01, 0x08),
                             0x10, 0x11, 0x13, *START_COMMANDS))
SCOPE = (
    "Controlled synchronous-functional comparison fixture. FLUSHE barriers "
    "change asynchronous VIF/VU event ordering; not the original BIOS event "
    "timeline or a BIOS/hardware oracle."
)


def serialize(packet: bytes) -> tuple[bytes, dict]:
    """Return the preserved stream plus start barriers and descriptive metadata.

    Raise ValueError on unknown commands, partial words/payloads or unsupported
    STCYCL. NUM=0 means 256 for MPG/UNPACK, and DIRECT IMM=0 means 65536 QWC.
    Zero-valued payload bytes remain data; they cannot be used to infer padding.
    Input length is not rounded up, even when a DMA consumer needs qword size.
    """
    if not isinstance(packet, bytes):
        raise TypeError("packet must be bytes")
    if len(packet) % 4:
        raise ValueError("VIF stream ends in a partial 32-bit word; no padding is added")

    result = bytearray()
    inserted = []
    command_count = 0
    cycle = 0  # The supported standalone fixture begins with Astra reset state.
    offset = 0
    while offset < len(packet):
        code = struct.unpack_from("<I", packet, offset)[0]
        command = code >> 24
        opcode = command & 0x7F
        payload_size = 0
        if opcode == 0x00 and command != 0x00:
            # Astra's NOP check is against the full command byte, unlike its
            # other supported commands. Do not silently admit IRQ-NOP here.
            raise ValueError(f"unsupported IRQ-NOP {code:08X} at 0x{offset:X}")
        if opcode == 0x01:  # STCYCL; refuse skip/fill modes immediately.
            cycle = code & 0xFFFF
            cl, wl = cycle & 0xFF, cycle >> 8
            if cl != wl:
                raise ValueError(f"noncontiguous STCYCL CL={cl}, WL={wl} at 0x{offset:X}")
        elif opcode == 0x20:  # STMASK
            payload_size = 4
        elif opcode in (0x30, 0x31):  # STROW / STCOL
            payload_size = 16
        elif opcode == 0x4A:  # MPG count is in 64-bit microinstruction pairs.
            payload_size = (((code >> 16) & 0xFF) or 256) * 8
        elif opcode == 0x50:  # DIRECT count is in GIF qwords.
            payload_size = ((code & 0xFFFF) or 65536) * 16
        elif opcode == 0x6C:  # Only unmasked V4-32, contiguous CL==WL.
            # No other UNPACK form is accepted: its source size/packing could
            # otherwise turn payload words into spurious start commands.
            cl, wl = cycle & 0xFF, cycle >> 8
            if cl != wl:
                raise ValueError(f"noncontiguous UNPACK at 0x{offset:X}")
            payload_size = (((code >> 16) & 0xFF) or 256) * 16
        elif opcode not in HEADER_COMMANDS:
            raise ValueError(f"unsupported VIF command {code:08X} at 0x{offset:X}")

        end = offset + 4 + payload_size
        if end > len(packet):
            available = len(packet) - offset - 4
            raise ValueError(
                f"truncated {code:08X} payload at 0x{offset:X}: "
                f"requires {payload_size} bytes, has {available}; no padding is added"
            )
        serialized_offset = len(result)
        result.extend(packet[offset:end])
        command_count += 1
        if opcode in START_COMMANDS:
            inserted.append({
                "original_offset": offset,
                "code": f"{code:08X}",
                "opcode": f"{opcode:02X}",
                "command": START_COMMANDS[opcode],
                "serialized_offset": serialized_offset,
                "barrier_offset": len(result),
            })
            result.extend(BARRIER)
        offset = end

    serialized = bytes(result)
    metadata = {
        "schema_version": 1,
        "mode": "controlled-synchronous-functional",
        "scope": SCOPE,
        "initial_state": "Astra reset STCYCL CL=WL=0; no prior partial command/payload",
        "original_bytes": len(packet),
        "serialized_bytes": len(serialized),
        "original_sha256": hashlib.sha256(packet).hexdigest(),
        "serialized_sha256": hashlib.sha256(serialized).hexdigest(),
        "command_count": command_count,
        "inserted_start_commands": inserted,
        "barrier_words": ["10000000", "00000000", "00000000", "00000000"],
    }
    return serialized, metadata


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path, help="original standalone VIF byte stream")
    parser.add_argument("output", type=Path, help="fresh serialized fixture filename")
    parser.add_argument("--metadata", "--json", dest="metadata", type=Path,
                        help="optional fresh JSON metadata filename")
    args = parser.parse_args()
    paths = [args.input, args.output]
    if args.metadata is not None:
        paths.append(args.metadata)
    if len({path.resolve() for path in paths}) != len(paths):
        parser.error("input, output and JSON metadata must use distinct paths")
    for path in paths[1:]:
        if path.exists() or path.is_symlink():
            parser.error(f"refusing to overwrite existing output: {path}")
    try:
        serialized, metadata = serialize(args.input.read_bytes())
    except (OSError, TypeError, ValueError) as error:
        parser.error(str(error))

    # Exclusive creation also prevents an output race from overwriting a file.
    # Reserve both outputs before writing, so an existing JSON target discovered
    # at this point cannot leave a successfully written binary without metadata.
    output_files = []
    try:
        for path in paths[1:]:
            output_files.append(path.open("xb"))
        output_files[0].write(serialized)
        if args.metadata is not None:
            output_files[1].write((json.dumps(metadata, indent=2) + "\n").encode("utf-8"))
    except OSError as error:
        parser.error(str(error))
    finally:
        for output_file in output_files:
            output_file.close()
    print(f"wrote controlled functional fixture {args.output}: "
          f"{len(serialized)} bytes, {len(metadata['inserted_start_commands'])} barriers; "
          "not the original BIOS event timeline or oracle")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
