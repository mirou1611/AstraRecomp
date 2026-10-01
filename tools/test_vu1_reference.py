#!/usr/bin/env python3
"""Independent boundary checks for the ELF wrapper and serial-dump grammar."""

import struct
import unittest

from make_vu1_reference_elf import (
    BEGIN_MARKER, END_MARKER, ELF_FILE_OFFSET, ENTRY, FLUSHE_QWORD,
    PACKET_ADDRESS, VU_MEMORY_SIZE, build, reset_register_packet,
)
from run_vu1_reference import parse_serial, compare_memory, select_prefix


class ReferenceElfTests(unittest.TestCase):
    def test_preserves_aligned_packet_and_appends_flush(self):
        packet = bytes(range(256)) * 15 + bytes(range(96))  # captured size: 3936
        image = build(packet)
        header = struct.unpack_from("<16sHHIIIIIHHHHHH", image)
        self.assertEqual(header[0][:7], b"\x7fELF\x01\x01\x01")
        self.assertEqual((header[1], header[2], header[4], header[5]), (2, 8, ENTRY, 52))
        segment = struct.unpack_from("<IIIIIIII", image, header[5])
        kind, offset, virtual, physical, filesz, memsz, flags, alignment = segment
        self.assertEqual((kind, offset, virtual, physical), (1, ELF_FILE_OFFSET, ENTRY, ENTRY))
        self.assertEqual(offset + filesz, len(image))
        self.assertEqual(filesz, memsz)
        self.assertEqual((offset - virtual) % alignment, 0)
        self.assertEqual(flags, 7)
        packet_offset = offset + PACKET_ADDRESS - virtual
        self.assertEqual(packet_offset % 16, 0)
        self.assertEqual(image[packet_offset:packet_offset + len(packet)], packet)
        self.assertEqual(image[packet_offset + len(packet):packet_offset + len(packet) + 16],
                         FLUSHE_QWORD)
        code = struct.unpack_from(f"<{(PACKET_ADDRESS - ENTRY) // 4}I", image, offset)
        self.assertIn(0x2409010B, code)  # preserved packet + reset prelude + FLUSHE
        self.assertIn(0x4849E800, code)  # poll architectural VPU_STAT
        self.assertIn(0x48C9E000, code)  # reset before packet

    def test_register_reset_prelude(self):
        packet = reset_register_packet()
        self.assertEqual(len(packet), 320)
        words = struct.unpack("<80I", packet)
        self.assertEqual(words[:2], (0, 0x4A260000))
        for destination in range(1, 32):
            self.assertEqual(words[2 * destination + 1],
                             (15 << 21) | (destination << 6) | 0x2C)
        self.assertEqual(words[-2:], (0x14000000, 0x10000000))

    def test_optional_vu1_reset(self):
        image = build(bytes(16), reset_vu1=False)
        code = struct.unpack_from("<256I", image, ELF_FILE_OFFSET)
        self.assertNotIn(0x48C9E000, code)

    def test_rejects_empty_unaligned_and_oversized_packets(self):
        for packet in (b"", bytes(15), bytes(0xFFFF * 16)):
            with self.subTest(size=len(packet)), self.assertRaises(ValueError):
                build(packet)


class SerialParserTests(unittest.TestCase):
    def setUp(self):
        self.rows = [f"VUMEM {offset:04X} {offset:08X} 89ABCDEF 80000000 FFFFFFFF"
                     for offset in range(0, VU_MEMORY_SIZE, 16)]
        self.lines = [BEGIN_MARKER, *self.rows, END_MARKER]

    def parse(self, lines):
        return parse_serial("\n".join(lines))

    def test_exact_size_endianness_and_console_prefix(self):
        result = self.parse(["startup message", *["[EE] " + line for line in self.lines]])
        self.assertEqual(len(result), 16384)
        self.assertEqual(result[:16], b"\0\0\0\0\xef\xcd\xab\x89\0\0\0\x80\xff\xff\xff\xff")
        self.assertEqual(struct.unpack_from("<I", result, 16368)[0], 16368)

    def test_missing_row(self):
        with self.assertRaises(ValueError):
            self.parse(self.lines[:500] + self.lines[501:])

    def test_duplicate_row(self):
        with self.assertRaises(ValueError):
            self.parse(self.lines[:500] + [self.lines[499]] + self.lines[500:])

    def test_out_of_order_row(self):
        lines = self.lines.copy()
        lines[2], lines[3] = lines[3], lines[2]
        with self.assertRaises(ValueError):
            self.parse(lines)

    def test_wrong_word_count_and_width(self):
        for replacement in ("VUMEM 0000 00000000 00000000 00000000",
                            "VUMEM 0000 0 00000000 00000000 00000000",
                            "VUMEM 0000 00000000 00000000 00000000 00000000 extra"):
            with self.subTest(row=replacement), self.assertRaises(ValueError):
                self.parse([BEGIN_MARKER, replacement, *self.rows[1:], END_MARKER])

    def test_missing_wrong_duplicate_and_premature_markers(self):
        cases = (self.lines[:-1], self.lines[1:],
                 self.lines[:-1] + ["VUMEM END 00002000"],
                 [BEGIN_MARKER, BEGIN_MARKER, *self.rows, END_MARKER],
                 [*self.lines, END_MARKER], [BEGIN_MARKER, *self.rows[:5], END_MARKER],
                 [END_MARKER, *self.lines], [*self.lines, self.rows[0]])
        for index, lines in enumerate(cases):
            with self.subTest(case=index), self.assertRaises(ValueError):
                self.parse(lines)


class MemoryComparisonTests(unittest.TestCase):
    def test_equal_and_word_qword_counts(self):
        reference = bytes(VU_MEMORY_SIZE)
        self.assertEqual(compare_memory(reference, reference)["different_words"], 0)
        actual = bytearray(reference)
        struct.pack_into("<I", actual, 4, 0x12345678)
        struct.pack_into("<I", actual, 12, 1)
        struct.pack_into("<I", actual, 32, 2)
        result = compare_memory(reference, actual)
        self.assertEqual((result["different_words"], result["different_qwords"]), (3, 2))
        self.assertEqual(result["examples"][0]["address"], "0000")
        self.assertEqual(result["examples"][0]["actual"][1], "12345678")

    def test_requires_complete_memory_and_bounds_examples(self):
        with self.assertRaises(ValueError):
            compare_memory(bytes(16383), bytes(16384))
        result = compare_memory(bytes(16384), bytes([1]) * 16384)
        self.assertEqual(len(result["examples"]), 32)
        self.assertTrue(result["examples_truncated"])

    def test_word_boundary_prefix_padding(self):
        packet = bytes(range(32))
        self.assertEqual(select_prefix(packet, None), packet)
        self.assertEqual(select_prefix(packet, 20), packet[:20] + bytes(12))
        for boundary in (-1, 0, 3, 33, 36):
            with self.subTest(boundary=boundary), self.assertRaises(ValueError):
                select_prefix(packet, boundary)


if __name__ == "__main__":
    unittest.main()
