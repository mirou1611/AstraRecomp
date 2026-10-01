#!/usr/bin/env python3
"""Packet-format, operand preservation and output-placement tests, not a float oracle."""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from make_vu1_math_fixture import boundary_cases, build, microprogram


# External raw values are repeated here so corruption in the generator's case
# table cannot also silently change this packet-preservation expectation.
RAW_OPERANDS = (
    (0x3F800000, 0x40200000), (0x3F800000, 0xC0200000),
    (0xBF800000, 0x40200000), (0xBF800000, 0xC0200000),
    (0x3F800000, 0x40400000), (0x3F800000, 0xC0400000),
    (0xBF800000, 0x40400000), (0xBF800000, 0xC0400000),
    (0x3F800000, 0x40000000), (0xBF800000, 0x40000000),
    (0xB063B75B, 0x42FECCCD), (0x8701CE82, 0x43D80D3E),
    (0x46FCC888, 0x43A0DA10), (0x793CC535, 0x43546E14),
    (0x3C23D70A, 0x3ECCCCCC), (0x3C23D70A, 0x3ECCCCCD),
)

RAW_BOUNDARY_OPERANDS = (
    (0x00800000, 0x3F800000), (0x00800000, 0x40000000),
    (0x00800000, 0xC0000000), (0x80800000, 0x40000000),
    (0x7F7FFFFF, 0x3F000000), (0xFF7FFFFF, 0x3F000000),
    (0x3F800000, 0x00800000), (0x3F000000, 0x00800000),
    (0x00800000, 0x7F7FFFFF), (0x00800000, 0x3F800001),
    (0x00800001, 0x3F800001), (0x3F800000, 0x3F800001),
    (0x3F7FFFFF, 0x3F800000), (0x3F7FFFFF, 0x3F800001),
    (0x3FA00000, 0x3FE00000), (0x3FE00000, 0x3FA00000),
)
BOUNDARY_PACKET_SHA256 = "17b3bf7f7ff11d800f6e58c48c8787b573b195e2b02f37d3a478da291a03f083"


def commands(packet):
    """Independently walk command payload lengths; reject unknown encodings."""
    offset = 0
    result = []
    while offset < len(packet):
        command = struct.unpack_from("<I", packet, offset)[0]
        opcode = command >> 24
        size = 0
        if opcode == 0x6C:
            size = ((command >> 16) & 255) * 16
        elif opcode == 0x4A:
            size = ((command >> 16) & 255) * 8
        elif opcode not in (0, 1, 5, 0x10, 0x14):
            raise ValueError(f"unsupported fixture command {command:08X}")
        if offset + 4 + size > len(packet):
            raise ValueError("truncated command payload")
        result.append((offset, command, packet[offset + 4:offset + 4 + size]))
        offset += 4 + size
    return result


class MathFixtureTests(unittest.TestCase):
    def setUp(self):
        self.packet, self.layout = build()

    def test_aligned_packet_and_complete_command_walk(self):
        self.assertEqual(len(self.packet), 2832)
        self.assertEqual(len(self.packet) % 16, 0)
        self.assertEqual(self.layout["packet_bytes"], len(self.packet))
        self.assertEqual(self.layout["packet_sha256"], hashlib.sha256(self.packet).hexdigest())
        self.assertEqual(self.layout["packet_sha256"],
                         "cf18eb048e058ba2179064c5fb3536c9a1a01bcb91800217efe07131ccb8159d")
        self.assertEqual(self.layout["suite"], "finite")
        parsed = commands(self.packet)
        self.assertEqual([entry[1] for entry in parsed[:4]],
                         [0x10000000, 0x01000101, 0x05000000, 0])
        for opcode in (0x6C, 0x4A, 0x14):
            self.assertEqual(sum(command >> 24 == opcode for _, command, _ in parsed), 16)
        self.assertEqual(sum(command == 0x10000000 for _, command, _ in parsed), 17)

    def test_raw_operands_are_preserved_at_absolute_input_addresses(self):
        unpack = [(offset, word, payload) for offset, word, payload in commands(self.packet)
                  if word >> 24 == 0x6C]
        for index, (lhs, rhs) in enumerate(RAW_OPERANDS):
            with self.subTest(index=index):
                offset, command, payload = unpack[index]
                self.assertEqual(command, 0x6C020100)
                self.assertEqual((offset + 4) % 16, 0)
                self.assertEqual(struct.unpack("<8I", payload), (lhs,) * 4 + (rhs,) * 4)
                entry = self.layout["cases"][index]
                self.assertEqual(entry["input_payload_offset"], offset + 4)
                self.assertEqual((entry["lhs"], entry["rhs"]), (f"{lhs:08X}", f"{rhs:08X}"))
                self.assertEqual(entry["input_qwords"], ["100", "101"])

    def test_each_program_loads_inputs_stores_distinct_output_and_ends(self):
        mpg = [(offset, command, payload) for offset, command, payload in commands(self.packet)
               if command >> 24 == 0x4A]
        outputs = []
        for index, (offset, command, payload) in enumerate(mpg):
            with self.subTest(index=index):
                self.assertEqual(command, 0x4A0D0000)
                self.assertEqual((offset + 4) % 8, 0)
                program = tuple(struct.iter_unpack("<II", payload))
                self.assertEqual(len(program), 13)
                self.assertEqual(program[0], (0x01E10100, 0x01E000EC))
                self.assertEqual(program[1], (0x01E20101, 0x000002FF))
                stores = [(slot, lower) for slot, (lower, _) in enumerate(program)
                          if lower >> 25 == 1]
                self.assertEqual(len(stores), 1)
                slot, store = stores[0]
                self.assertEqual(store, 0x03E01800 | index)
                outputs.append(store & 0x7FF)
                if index < 10:
                    self.assertEqual(program[5], (0x80020BBC, 0x000002FF))
                    self.assertEqual(program[6], (0x800003BF, 0x000002FF))
                    self.assertEqual(program[7], (0x8000033C, 0x002000DC))
                    self.assertGreaterEqual(slot - 7, 4)
                    self.assertEqual(self.layout["cases"][index]["result_lanes"], "w")
                else:
                    self.assertEqual(program[5], (0x8000033C, 0x01E208EA))
                    self.assertGreaterEqual(slot - 5, 4)
                self.assertEqual(program[11][1], 0x400002FF)
                self.assertEqual(program[12], (0x8000033C, 0x000002FF))
                self.assertFalse(any(upper & 0x40000000 for _, upper in program[:11]))
                entry = self.layout["cases"][index]
                self.assertEqual(entry["output_qword"], f"{index:03X}")
                self.assertEqual(entry["output_byte_offset"], index * 16)
        self.assertEqual(outputs, list(range(16)))
        self.assertTrue(set(outputs).isdisjoint((0x100, 0x101)))

    def test_every_launch_is_address_zero_and_followed_by_flush(self):
        parsed = commands(self.packet)
        for index, (offset, command, _) in enumerate(parsed):
            if command >> 24 != 0x14:
                continue
            self.assertEqual(command, 0x14000000)
            self.assertEqual(parsed[index + 1][1], 0x10000000)
            case = next(entry for entry in self.layout["cases"]
                        if entry["mscal_command_offset"] == offset)
            self.assertEqual(case["micro_pairs"], 13)

    def test_only_pinned_hardware_coproduct_rows_have_recorded_values(self):
        recorded = [(index, entry["recorded_vu0_muli"])
                    for index, entry in enumerate(self.layout["cases"])
                    if "recorded_vu0_muli" in entry]
        self.assertEqual(recorded, [(10, "B3E2A618"), (11, "8B5B19E9"),
                                    (12, "4B1ED4A7"), (13, "7D1CA47B")])
        for index, _ in recorded:
            self.assertIn("97469ffbed8631277b94e28d01dabd702aa97ef3",
                          self.layout["cases"][index]["recorded_source"])
            self.assertIn("VU0 MULi", self.layout["cases"][index]["recorded_scope"])
            self.assertIn("not full hardware equivalence", self.layout["cases"][index]["recorded_scope"])

    def test_invalid_program_requests(self):
        for operation, address in (("madd", 0), ("div", -1), ("mul", 16)):
            with self.subTest(operation=operation, address=address), self.assertRaises(ValueError):
                microprogram(operation, address)

    def test_cli_binary_and_json_layout(self):
        generator = Path(__file__).with_name("make_vu1_math_fixture.py")
        with tempfile.TemporaryDirectory(prefix="vu1-math-test-") as directory:
            packet = Path(directory) / "fixture.bin"
            layout = Path(directory) / "fixture.json"
            completed = subprocess.run([sys.executable, str(generator), str(packet),
                                        "--layout", str(layout)], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(packet.read_bytes(), self.packet)
            self.assertEqual(json.loads(layout.read_text(encoding="utf-8")), self.layout)


    def test_boundary_cases_preserve_raw_operands_and_output_layout(self):
        packet, layout = build("div-boundaries")
        self.assertEqual(layout["suite"], "div-boundaries")
        self.assertEqual(len(packet) % 16, 0)
        self.assertEqual(len(layout["cases"]), 16)
        self.assertEqual(tuple((case.lhs, case.rhs) for case in boundary_cases()),
                         RAW_BOUNDARY_OPERANDS)
        self.assertTrue(all(case.operation == "div" and case.recorded_vu0_muli is None
                            for case in boundary_cases()))
        unpack = [(offset, word, payload) for offset, word, payload in commands(packet)
                  if word >> 24 == 0x6C]
        self.assertEqual(len(unpack), 16)
        for index, (lhs, rhs) in enumerate(RAW_BOUNDARY_OPERANDS):
            with self.subTest(index=index):
                offset, command, payload = unpack[index]
                self.assertEqual(command, 0x6C020100)
                self.assertEqual((offset + 4) % 16, 0)
                self.assertEqual(struct.unpack("<8I", payload), (lhs,) * 4 + (rhs,) * 4)
                entry = layout["cases"][index]
                self.assertEqual((entry["lhs"], entry["rhs"]), (f"{lhs:08X}", f"{rhs:08X}"))
                self.assertEqual(entry["input_payload_offset"], offset + 4)
                self.assertEqual(entry["output_qword"], f"{index:03X}")
                self.assertEqual(entry["output_byte_offset"], index * 16)
                self.assertEqual(entry["result_lanes"], "w")
                self.assertNotIn("recorded_vu0_muli", entry)
        with self.assertRaises(ValueError):
            build("unknown")

    def test_cli_boundary_suite_reproduces_captured_packet_hash(self):
        generator = Path(__file__).with_name("make_vu1_math_fixture.py")
        with tempfile.TemporaryDirectory(prefix="vu1-div-boundary-test-") as directory:
            packet = Path(directory) / "boundaries.bin"
            layout_path = Path(directory) / "boundaries.json"
            completed = subprocess.run(
                [sys.executable, str(generator), str(packet), "--suite", "div-boundaries",
                 "--layout", str(layout_path)], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(hashlib.sha256(packet.read_bytes()).hexdigest(), BOUNDARY_PACKET_SHA256)
            expected_packet, expected_layout = build("div-boundaries")
            self.assertEqual(packet.read_bytes(), expected_packet)
            self.assertEqual(json.loads(layout_path.read_text(encoding="utf-8")), expected_layout)
            self.assertEqual(expected_layout["packet_sha256"], BOUNDARY_PACKET_SHA256)


if __name__ == "__main__":
    unittest.main()
