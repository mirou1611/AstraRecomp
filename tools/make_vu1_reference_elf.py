#!/usr/bin/env python3
"""Wrap a qword-aligned VIF1 capture in a standalone EE memory-dump ELF.

The supplied packet is preserved byte for byte. One FLUSHE/NOP qword is
appended; an optional register-initialization microprogram precedes it. The
EE waits for DMA STR and VPU_STAT's VU1 busy bit to clear
before reading all 16 KiB of VU1 data memory through the EE mapping.

This is independently hand-encoded fixture code, with no PS2SDK dependency.
Register/interface facts were checked against official PCSX2 v2.8.2 sources:
  https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Memory.cpp
  https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/HwWrite.cpp
  https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/VU0.cpp
  https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/VU.h
  https://github.com/PCSX2/pcsx2/blob/v2.8.2/pcsx2/Vif_Codes.cpp
Only architectural constants and behavior are used, not emulator source code.
The dump records final memory, not historical GIF/XGKICK deliveries.
"""

import argparse
import hashlib
import struct
from pathlib import Path

ENTRY = 0x00100000
PACKET_ADDRESS = 0x00101000
ELF_FILE_OFFSET = 0x1000
VU_MEMORY_SIZE = 0x4000
FLUSHE_QWORD = struct.pack("<IIII", 0x10000000, 0, 0, 0)
BEGIN_MARKER = "VUMEM BEGIN 00004000"
END_MARKER = "VUMEM END 00004000"


def reset_register_packet():
    """Initialize VU1 registers without relying on reset clearing VF/VI."""
    pairs = []
    for destination in range(1, 32):
        lower = ((0x08 << 25) | (destination << 16)) if destination < 16 else 0x8000033C
        upper = (15 << 21) | (destination << 6) | 0x2C  # SUB vfN,vf0,vf0
        pairs.extend((lower, upper))
    pairs.extend((0x818003BC, (15 << 21) | (6 << 6) | 0x3C))  # DIV 0/1; MULA vf0,vf0.x
    pairs.extend((0x800003BF, 0x000002FF))  # WAITQ
    pairs.extend((0, 0x800002FF))  # I = 0
    for index in range(4):
        pairs.extend((0x8000033C, 0x28 | (0x40000000 if index == 2 else 0)))
    # NOP/MPG, 38 pairs, MSCAL/FLUSHE; exactly twenty DMA qwords.
    return struct.pack("<" + "I" * (len(pairs) + 4),
                       0, 0x4A000000 | ((len(pairs) // 2) << 16),
                       *pairs, 0x14000000, 0x10000000)

# MIPS register numbers used by the small fixture.
ZERO, A0, A1 = 0, 4, 5
T0, T1, T2, T3 = 8, 9, 10, 11
S0, S1, S2, S3, S4, S5, S6, RA = 16, 17, 18, 19, 20, 21, 22, 31


def immediate(op, rs, rt, value):
    return (op << 26) | (rs << 21) | (rt << 16) | (value & 0xFFFF)


def register(rs, rt, rd, shift, function):
    return (rs << 21) | (rt << 16) | (rd << 11) | (shift << 6) | function


class Assembler:
    """Only labels and the branch/call relocations needed by this fixture."""

    def __init__(self):
        self.words = []
        self.labels = {}
        self.fixups = []

    def emit(self, word):
        self.words.append(word)

    def label(self, name):
        if name in self.labels:
            raise ValueError(f"duplicate label {name}")
        self.labels[name] = len(self.words)

    def load(self, rt, value):
        self.emit(immediate(0x0F, ZERO, rt, value >> 16))  # lui
        self.emit(immediate(0x0D, rt, rt, value))  # ori

    def branch(self, op, rs, rt, name):
        self.fixups.append((len(self.words), name, "branch"))
        self.emit(immediate(op, rs, rt, 0))
        self.emit(0)  # delay slot

    def call(self, name):
        self.fixups.append((len(self.words), name, "call"))
        self.emit(0x0C000000)  # jal
        self.emit(0)  # delay slot

    def finish(self):
        words = self.words.copy()
        for at, name, kind in self.fixups:
            target = self.labels[name]
            if kind == "branch":
                displacement = target - at - 1
                if not -0x8000 <= displacement <= 0x7FFF:
                    raise ValueError("branch displacement out of range")
                words[at] |= displacement & 0xFFFF
            else:
                words[at] |= ((ENTRY + target * 4) >> 2) & 0x03FFFFFF
        return struct.pack(f"<{len(words)}I", *words)


def build(packet: bytes, reset_vu1: bool = True) -> bytes:
    if not packet or len(packet) % 16:
        raise ValueError("VIF1 packet must be nonempty and aligned to 16-byte qwords")
    if len(packet) // 16 + 1 > 0xFFFF:
        raise ValueError("VIF1 packet plus FLUSHE exceeds the DMA QWC field")
    prelude = reset_register_packet() if reset_vu1 else b""
    transfer = prelude + packet + FLUSHE_QWORD
    if len(transfer) // 16 > 0xFFFF:
        raise ValueError("VIF1 packet plus initialization/FLUSHE exceeds the DMA QWC field")
    transfer_address = PACKET_ADDRESS - len(prelude)
    string_address = (PACKET_ADDRESS + len(packet) + len(FLUSHE_QWORD) + 15) & ~15
    strings = bytearray()
    addresses = {}
    for name, value in (
        ("begin", BEGIN_MARKER + "\n"),
        ("row", "VUMEM "),
        ("end", END_MARKER + "\n"),
        ("hex", "0123456789ABCDEF"),
    ):
        addresses[name] = string_address + len(strings)
        strings.extend(value.encode("ascii") + b"\0")

    a = Assembler()
    # Enable COP2 explicitly instead of relying on BIOS entry state.
    a.emit(0x40086000)  # mfc0 t0, Status
    a.load(T1, 0x40000000)  # Status.CU2
    a.emit(register(T0, T1, T0, 0, 0x25))  # or t0, t0, t1
    a.load(T1, 0xFFFEFFFE)  # disable IE and EIE for the standalone fixture
    a.emit(register(T0, T1, T0, 0, 0x24))  # and t0, t0, t1
    a.emit(0x40886000)  # mtc0 t0, Status
    a.emit(0)
    a.emit(0)
    if reset_vu1:
        a.emit(immediate(9, ZERO, T1, 0x200))
        a.emit(0x48C9E000)  # ctc2 t1, VI28/FBRST (VU1 reset)
        a.emit(0)

    # VU reset does not clear its data bank. Match Astra's reset-memory replay
    # explicitly, without depending on firmware or loader entry contents.
    a.load(T0, 0x1100C000)
    a.load(T2, 0x11010000)
    a.label("clear_vu_data")
    a.emit(immediate(0x2B, T0, ZERO, 0))
    a.emit(immediate(9, T0, T0, 4))
    a.branch(5, T0, T2, "clear_vu_data")

    a.load(T0, 0x1000E000)  # D_CTRL
    a.emit(immediate(9, ZERO, T1, 1))
    a.emit(immediate(0x2B, T0, T1, 0))  # enable DMA
    a.load(T0, 0x10009000)  # D1_CHCR (VIF1)
    a.load(T1, transfer_address)
    a.emit(immediate(0x2B, T0, T1, 0x10))  # D1_MADR
    a.emit(immediate(9, ZERO, T1, len(transfer) // 16))
    a.emit(immediate(0x2B, T0, T1, 0x20))  # D1_QWC
    a.emit(immediate(9, ZERO, T1, 0x101))  # STR, memory -> VIF1, normal mode
    a.emit(immediate(0x2B, T0, T1, 0))
    a.label("wait_dma")
    a.emit(immediate(0x23, T0, T1, 0))  # lw t1, D1_CHCR
    a.emit(immediate(0x0C, T1, T1, 0x100))
    a.branch(5, T1, ZERO, "wait_dma")
    a.label("wait_vu1")
    a.emit(0x4849E800)  # cfc2 t1, VI29/VPU_STAT
    a.emit(immediate(0x0C, T1, T1, 0x100))
    a.branch(5, T1, ZERO, "wait_vu1")

    a.load(S0, 0x1000F180)  # EE SIO TXFIFO
    a.load(S1, 0x1100C000)  # EE mapping of VU1 data memory
    a.emit(immediate(9, ZERO, S2, 0))
    a.emit(immediate(9, ZERO, S3, VU_MEMORY_SIZE))
    a.load(S4, addresses["hex"])
    a.emit(immediate(9, ZERO, S5, ord(" ")))
    a.emit(immediate(9, ZERO, S6, ord("\n")))
    a.load(A0, addresses["begin"])
    a.call("print_string")
    a.label("row")
    a.load(A0, addresses["row"])
    a.call("print_string")
    a.emit(register(S2, ZERO, A0, 0, 0x25))  # or a0, s2, zero
    a.emit(immediate(9, ZERO, A1, 4))
    a.call("print_hex")
    for offset in (0, 4, 8, 12):
        a.emit(immediate(0x28, S0, S5, 0))  # sb space, TXFIFO
        a.emit(immediate(0x23, S1, A0, offset))
        a.emit(immediate(9, ZERO, A1, 8))
        a.call("print_hex")
    a.emit(immediate(0x28, S0, S6, 0))
    a.emit(immediate(9, S1, S1, 16))
    a.emit(immediate(9, S2, S2, 16))
    a.branch(5, S2, S3, "row")
    a.load(A0, addresses["end"])
    a.call("print_string")
    a.label("done")
    a.branch(4, ZERO, ZERO, "done")

    a.label("print_string")
    a.emit(immediate(0x24, A0, T0, 0))  # lbu
    a.branch(4, T0, ZERO, "string_return")
    a.emit(immediate(0x28, S0, T0, 0))
    a.emit(immediate(9, A0, A0, 1))
    a.branch(4, ZERO, ZERO, "print_string")
    a.label("string_return")
    a.emit(register(RA, ZERO, ZERO, 0, 8))  # jr ra
    a.emit(0)

    a.label("print_hex")
    a.emit(immediate(9, A1, T0, -1))
    a.emit(register(ZERO, T0, T0, 2, 0))  # sll t0, t0, 2
    a.label("hex_digit")
    a.emit(register(T0, A0, T1, 0, 6))  # srlv t1, a0, t0
    a.emit(immediate(0x0C, T1, T1, 15))
    a.emit(register(S4, T1, T2, 0, 0x21))  # addu t2, s4, t1
    a.emit(immediate(0x24, T2, T3, 0))
    a.emit(immediate(0x28, S0, T3, 0))
    a.emit(immediate(9, T0, T0, -4))
    a.branch(1, T0, 1, "hex_digit")  # bgez t0
    a.emit(register(RA, ZERO, ZERO, 0, 8))
    a.emit(0)
    code = a.finish()
    if ENTRY + len(code) > transfer_address:
        raise ValueError("fixture code overlaps the VIF1 packet")

    segment_size = string_address + len(strings) - ENTRY
    image = bytearray(ELF_FILE_OFFSET + segment_size)
    image[:52] = struct.pack(
        "<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + bytes(9),
        2, 8, 1, ENTRY, 52, 0, 0, 52, 32, 1, 0, 0, 0,
    )
    image[52:84] = struct.pack(
        "<IIIIIIII", 1, ELF_FILE_OFFSET, ENTRY, ENTRY,
        segment_size, segment_size, 7, 0x1000,
    )
    image[ELF_FILE_OFFSET:ELF_FILE_OFFSET + len(code)] = code
    packet_offset = ELF_FILE_OFFSET + transfer_address - ENTRY
    image[packet_offset:packet_offset + len(transfer)] = transfer
    string_offset = ELF_FILE_OFFSET + string_address - ENTRY
    image[string_offset:string_offset + len(strings)] = strings
    return bytes(image)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("packet", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--no-reset-vu1", action="store_true",
                        help="retain BIOS VU1 register entry state (default resets VU1)")
    args = parser.parse_args()
    if args.packet.resolve() == args.output.resolve():
        parser.error("ELF output must not overwrite the supplied VIF packet")
    packet = args.packet.read_bytes()
    image = build(packet, reset_vu1=not args.no_reset_vu1)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(image)
    print(f"wrote {len(image)} bytes to {args.output}; "
          f"packet bytes={len(packet)} sha256={hashlib.sha256(packet).hexdigest()}")


if __name__ == "__main__":
    main()
