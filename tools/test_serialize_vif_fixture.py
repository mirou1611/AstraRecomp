#!/usr/bin/env python3
"""Command-boundary, preservation and fail-closed tests; not a BIOS oracle."""

import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

from serialize_vif_fixture import BARRIER, serialize


def words(*values):
    return struct.pack(f"<{len(values)}I", *values)


class SerializeFixtureTests(unittest.TestCase):
    def test_three_start_commands_have_exact_barriers_and_original_metadata(self):
        original = words(0x14000123, 0x15000045, 0x17000000, 0)
        result, metadata = serialize(original)
        self.assertEqual(result, words(0x14000123) + BARRIER +
                         words(0x15000045) + BARRIER +
                         words(0x17000000) + BARRIER + words(0))
        self.assertEqual(metadata["command_count"], 4)
        self.assertEqual(metadata["original_bytes"], 16)
        self.assertEqual(metadata["serialized_bytes"], 64)
        self.assertEqual(metadata["original_sha256"], hashlib.sha256(original).hexdigest())
        self.assertEqual(metadata["serialized_sha256"], hashlib.sha256(result).hexdigest())
        self.assertEqual(metadata["inserted_start_commands"], [
            {"original_offset": 0, "code": "14000123", "opcode": "14", "command": "MSCAL",
             "serialized_offset": 0, "barrier_offset": 4},
            {"original_offset": 4, "code": "15000045", "opcode": "15", "command": "MSCALF",
             "serialized_offset": 20, "barrier_offset": 24},
            {"original_offset": 8, "code": "17000000", "opcode": "17", "command": "MSCNT",
             "serialized_offset": 40, "barrier_offset": 44},
        ])
        self.assertIn("not the original BIOS event timeline", metadata["scope"])

    def test_upload_payload_opcodes_and_nops_are_not_commands(self):
        # Independent payload sizes: STMASK=4, STROW/STCOL=16, MPG NUM2=16,
        # DIRECT IMM1=16, V4-32 NUM1=16. Every payload looks like VIF starts.
        fake_commands = words(0x14000000, 0x15000000, 0x17000000, 0)
        original = (words(0x01000404, 0x20000000, 0x14000000) +
                    words(0x30000000) + fake_commands +
                    words(0x31000000) + fake_commands +
                    words(0x4A020000) + fake_commands +
                    words(0x50000001) + fake_commands +
                    words(0x6C018000) + fake_commands + words(0x17000000, 0))
        result, metadata = serialize(original)
        self.assertEqual(metadata["command_count"], 9)
        self.assertEqual(len(metadata["inserted_start_commands"]), 1)
        original_start = len(original) - 8
        self.assertEqual(metadata["inserted_start_commands"][0]["original_offset"], original_start)
        self.assertEqual(result, original[:-4] + BARRIER + original[-4:])

    def test_every_original_byte_is_preserved_in_order_and_modulo_16_alignment(self):
        original = (words(0x14000001, 0x6C018000) + bytes(range(16)) +
                    words(0, 0x15000002, 0x4A010000) + bytes(range(8)) +
                    words(0x17000000, 0, 0, 0, 0))
        self.assertEqual(len(original) % 16, 0)
        result, metadata = serialize(original)
        old_cursor = new_cursor = 0
        restored = bytearray()
        for entry in metadata["inserted_start_commands"]:
            end = entry["original_offset"] + 4
            preserved = original[old_cursor:end]
            self.assertEqual(result[new_cursor:new_cursor + len(preserved)], preserved)
            for index in range(len(preserved)):
                self.assertEqual((old_cursor + index) % 16, (new_cursor + index) % 16)
            restored.extend(result[new_cursor:new_cursor + len(preserved)])
            new_cursor += len(preserved)
            self.assertEqual(new_cursor, entry["barrier_offset"])
            self.assertEqual(result[new_cursor:new_cursor + 16], words(0x10000000, 0, 0, 0))
            new_cursor += 16
            old_cursor = end
        self.assertEqual(result[new_cursor:], original[old_cursor:])
        self.assertEqual(new_cursor % 16, old_cursor % 16)
        restored.extend(result[new_cursor:])
        self.assertEqual(bytes(restored), original)
        self.assertEqual(len(result) - len(original), 3 * 16)
        self.assertEqual(len(result) % 16, 0)

    def test_payload_free_subset_is_retained_without_insertions(self):
        opcodes = (0, 1, 2, 3, 4, 5, 6, 7, 0x10, 0x11, 0x13)
        original = b"".join(words((opcode << 24) | (0x0202 if opcode == 1 else 0x1234))
                            for opcode in opcodes)
        result, metadata = serialize(original)
        self.assertEqual(result, original)
        self.assertEqual(metadata["command_count"], len(opcodes))
        self.assertEqual(metadata["inserted_start_commands"], [])

    def test_reset_and_equal_cycle_modes_allow_v4_32(self):
        for prefix in (b"", words(0x01000000), words(0x01000101), words(0x0100FFFF)):
            with self.subTest(prefix=prefix.hex()):
                original = prefix + words(0x6C010000) + words(0, 0, 0, 0)
                self.assertEqual(serialize(original)[0], original)

    def test_noncontiguous_cycle_is_rejected_even_without_unpack(self):
        for code in (0x01000102, 0x01000201, 0x01000100, 0x01000001):
            with self.subTest(code=f"{code:08X}"), self.assertRaisesRegex(ValueError, "noncontiguous"):
                serialize(words(code, 0, 0, 0))

    def test_other_unpack_forms_and_unknown_commands_are_rejected(self):
        for opcode in (0x12, 0x16, 0x21, 0x4B, 0x51, 0x60, 0x68, 0x6D, 0x7C):
            with self.subTest(opcode=opcode), self.assertRaisesRegex(ValueError, "unsupported"):
                serialize(words(opcode << 24, 0, 0, 0))

    def test_irq_start_bit_is_retained_and_decoded_but_irq_nop_is_rejected(self):
        for command in (0x94000123, 0x95000045, 0x97000000):
            with self.subTest(command=f"{command:08X}"):
                result, metadata = serialize(words(command))
                self.assertEqual(result, words(command) + BARRIER)
                self.assertEqual(metadata["inserted_start_commands"][0]["code"], f"{command:08X}")
        with self.assertRaisesRegex(ValueError, "IRQ-NOP"):
            serialize(words(0x80000000))

    def test_num_zero_is_256_for_mpg_and_unpack(self):
        for command, payload_bytes in ((0x4A000000, 256 * 8), (0x6C000000, 256 * 16)):
            with self.subTest(command=f"{command:08X}"):
                payload = words(0x14000000) * (payload_bytes // 4)
                original = words(command) + payload + words(0x17000000, 0, 0)
                result, metadata = serialize(original)
                self.assertEqual(metadata["command_count"], 4)
                self.assertEqual(len(metadata["inserted_start_commands"]), 1)
                self.assertEqual(result, original[:-8] + BARRIER + original[-8:])
                self.assertEqual(metadata["inserted_start_commands"][0]["original_offset"],
                                 payload_bytes + 4)

    def test_direct_imm_zero_is_65536_qwords(self):
        payload = words(0x17000000) * (65536 * 4)
        original = words(0x50000000) + payload + words(0x14000000)
        result, metadata = serialize(original)
        self.assertEqual(result, original + BARRIER)
        self.assertEqual(metadata["command_count"], 2)
        self.assertEqual(len(metadata["inserted_start_commands"]), 1)
        self.assertEqual(metadata["inserted_start_commands"][0]["original_offset"], 4 + len(payload))

    def test_truncated_declared_payloads_are_never_filled_with_padding(self):
        for command, required in ((0x20000000, 4), (0x30000000, 16), (0x31000000, 16),
                                  (0x4A010000, 8), (0x6C010000, 16), (0x50000001, 16),
                                  (0x4A000000, 2048), (0x6C000000, 4096),
                                  (0x50000000, 1048576)):
            for available in (0, required - 4):
                with self.subTest(command=f"{command:08X}", available=available):
                    with self.assertRaisesRegex(ValueError, "truncated.*no padding"):
                        serialize(words(command) + b"\x00" * available)

    def test_nops_cannot_pad_a_num_zero_upload_or_expose_a_fake_start(self):
        # These four words would be a valid short payload for NUM1, but NUM0
        # declares 4096 bytes. They are data, not padding/commands to recover.
        with self.assertRaisesRegex(ValueError, "requires 4096 bytes, has 16"):
            serialize(words(0x6C000000, 0, 0, 0x14000000, 0))

    def test_partial_words_rejected_without_rounding(self):
        for suffix in (b"\x00", b"\x00\x00", b"\x00\x00\x00"):
            with self.subTest(size=len(suffix)), self.assertRaisesRegex(ValueError, "partial"):
                serialize(words(0x14000000) + suffix)

    def test_empty_stream_and_word_aligned_stream_are_not_padded(self):
        result, metadata = serialize(b"")
        self.assertEqual(result, b"")
        self.assertEqual(metadata["command_count"], 0)
        self.assertEqual(serialize(words(0))[0], words(0))

    def test_cli_writes_fresh_binary_and_optional_metadata(self):
        script = Path(__file__).with_name("serialize_vif_fixture.py")
        original = words(0, 0x14000000, 0, 0)
        expected, metadata = serialize(original)
        with tempfile.TemporaryDirectory(prefix="vif-serialize-test-") as directory:
            source = Path(directory) / "original.bin"
            output = Path(directory) / "controlled.bin"
            layout = Path(directory) / "controlled.json"
            source.write_bytes(original)
            completed = subprocess.run([sys.executable, str(script), str(source), str(output),
                                        "--json", str(layout)], capture_output=True, text=True)
            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(output.read_bytes(), expected)
            self.assertEqual(source.read_bytes(), original)
            self.assertEqual(json.loads(layout.read_text(encoding="utf-8")), metadata)
            self.assertIn("not the original BIOS event timeline or oracle", completed.stdout)

    def test_cli_refuses_collisions_and_preserves_existing_files(self):
        script = Path(__file__).with_name("serialize_vif_fixture.py")
        with tempfile.TemporaryDirectory(prefix="vif-serialize-test-") as directory:
            source = Path(directory) / "original.bin"
            output = Path(directory) / "controlled.bin"
            layout = Path(directory) / "controlled.json"
            original = words(0x14000000, 0, 0, 0)
            source.write_bytes(original)
            for destination, json_path in ((source, None), (output, source), (output, output)):
                with self.subTest(destination=destination, json_path=json_path):
                    arguments = [sys.executable, str(script), str(source), str(destination)]
                    if json_path is not None:
                        arguments.extend(("--metadata", str(json_path)))
                    completed = subprocess.run(arguments, capture_output=True, text=True)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn("distinct paths", completed.stderr)
                    self.assertEqual(source.read_bytes(), original)
                    self.assertFalse(output.exists())
            for existing in (output, layout):
                with self.subTest(existing=existing):
                    existing.write_bytes(b"KEEP")
                    completed = subprocess.run([sys.executable, str(script), str(source), str(output),
                                                "--metadata", str(layout)], capture_output=True, text=True)
                    self.assertNotEqual(completed.returncode, 0)
                    self.assertIn("refusing to overwrite", completed.stderr)
                    self.assertEqual(existing.read_bytes(), b"KEEP")
                    self.assertEqual(source.read_bytes(), original)
                    if existing == output:
                        self.assertFalse(layout.exists())
                    else:
                        self.assertFalse(output.exists())
                    existing.unlink()

    def test_cli_rejects_bad_input_before_creating_outputs(self):
        script = Path(__file__).with_name("serialize_vif_fixture.py")
        with tempfile.TemporaryDirectory(prefix="vif-serialize-test-") as directory:
            source = Path(directory) / "original.bin"
            output = Path(directory) / "controlled.bin"
            layout = Path(directory) / "controlled.json"
            source.write_bytes(words(0x6C000000, 0, 0, 0))
            completed = subprocess.run([sys.executable, str(script), str(source), str(output),
                                        "--metadata", str(layout)], capture_output=True, text=True)
            self.assertNotEqual(completed.returncode, 0)
            self.assertIn("truncated", completed.stderr)
            self.assertFalse(output.exists())
            self.assertFalse(layout.exists())


if __name__ == "__main__":
    unittest.main()
