#!/usr/bin/env python3
"""Strict diagnostic configuration and observational NOP-ROM subprocess tests.

This fixture owns its temporary 4 MiB synthetic ROM and PPMs. It does not need
Sony firmware and is not a GS rendering or PS2 boot-accuracy oracle.
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


TRACE_BINARY = None


class BiosTraceWatchTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="astra-gs-watch-")
        cls.directory = Path(cls.temporary.name)
        cls.rom = cls.directory / "owned-nop-rom.bin"
        cls.rom.write_bytes(bytes(4 * 1024 * 1024))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_trace(self, overrides=None, max_steps=8, framebuffer=None, stop_pc=0,
                  words=None):
        environment = os.environ.copy()
        for key in ("ASTRA_TRACE_SECONDS", "ASTRA_STOP_ON_SPRITE",
                    "ASTRA_GS_WATCH", "ASTRA_GS_FRAME_STEPS"):
            environment.pop(key, None)
        environment.update(overrides or {})
        rom = self.rom
        if words is not None:
            rom = self.directory / "owned-register-rom.bin"
            program = struct.pack(f"<{len(words)}I", *words)
            rom.write_bytes(program + bytes(4 * 1024 * 1024 - len(program)))
        arguments = [str(TRACE_BINARY), str(rom), str(stop_pc), str(max_steps),
                     "1", "0", "8", "0", "0", "0", "-"]
        if framebuffer is not None:
            arguments.append(str(framebuffer))
        return subprocess.run(arguments, env=environment, text=True,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=15, check=False)

    def assert_rejected(self, variable, value, max_steps=8, framebuffer=None):
        result = self.run_trace({variable: value}, max_steps, framebuffer)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn(variable, result.stderr)
        self.assertNotIn("steps=", result.stdout)
        self.assertNotIn("gs_pixel_write", result.stdout)

    def test_watch_parser_rejects_invalid_or_ambiguous_values(self):
        invalid = ("", " ", " 0", "0 ", "+0", "-4", "0x", "0X", "g",
                   "0,,4", ",0", "0,", "0, 4", "0\t,4", "1", "3FFFFD",
                   "400000", "FFFFFFFFFFFFFFFF", "10000000000000000",
                   "0,0", "0,0x0", "4,0X04", "0;4",
                   ",".join(f"{index * 4:X}" for index in range(17)))
        for value in invalid:
            with self.subTest(value=value):
                self.assert_rejected("ASTRA_GS_WATCH", value)

    def test_frame_parser_rejects_invalid_or_out_of_range_steps(self):
        framebuffer = self.directory / "invalid-step.ppm"
        invalid = ("", " ", " 1", "1 ", "+1", "-1", "0", "0x1", "1a",
                   ",1", "1,", "1,,2", "1, 2", "2,1", "1,1", "9",
                   "18446744073709551616", "100000000000000000000000000000")
        for value in invalid:
            with self.subTest(value=value):
                self.assert_rejected("ASTRA_GS_FRAME_STEPS", value,
                                     framebuffer=framebuffer)
        self.assert_rejected("ASTRA_GS_FRAME_STEPS",
                             ",".join(str(index) for index in range(1, 18)),
                             max_steps=32, framebuffer=framebuffer)
        self.assertFalse(framebuffer.exists())
        self.assertEqual(list(self.directory.glob("invalid-step.ppm.step-*.ppm")), [])

    def test_step_capture_requires_explicit_framebuffer_path(self):
        self.assert_rejected("ASTRA_GS_FRAME_STEPS", "1")

    def test_watch_enabled_is_observational_with_no_raster_writes(self):
        baseline = self.run_trace()
        watched = self.run_trace({"ASTRA_GS_WATCH": "0x0,0X3FFFFC"})
        self.assertEqual(baseline.returncode, 1, baseline.stderr)
        self.assertEqual(watched.returncode, baseline.returncode, watched.stderr)
        self.assertIn("steps=8 reason=running pc=BFC00020", baseline.stdout)
        self.assertNotIn("gs_pixel_watch", baseline.stdout)
        self.assertNotIn("gs_pixel_write", watched.stdout)
        self.assertIn("gs_pixel_watch_scope=raster-only logical-linear-vram",
                      watched.stdout)
        summaries = re.findall(r"^gs_pixel_watch address=(\w+) total=0 retained=0 "
                               r"dropped=0 latest_raw=00000000$",
                               watched.stdout, re.MULTILINE)
        self.assertEqual(summaries, ["00000000", "003FFFFC"])
        non_diagnostic = "\n".join(
            line for line in watched.stdout.splitlines()
            if not line.startswith("gs_pixel_watch")) + "\n"
        self.assertEqual(non_diagnostic, baseline.stdout)
        self.assertEqual(watched.stderr, baseline.stderr)

    def test_hex_is_not_implicitly_octal_and_accepts_full_watch_capacity(self):
        result = self.run_trace({"ASTRA_GS_WATCH": "010,0xA0,0XffC"})
        self.assertEqual(result.returncode, 1, result.stderr)
        for address in ("00000010", "000000A0", "00000FFC"):
            self.assertIn(f"gs_pixel_watch address={address} total=0", result.stdout)
        result = self.run_trace({"ASTRA_GS_WATCH":
                                 ",".join(f"{index * 4:X}" for index in range(16))})
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertEqual(len(re.findall(r"^gs_pixel_watch address=", result.stdout,
                                        re.MULTILINE)), 16)

    def test_snapshots_are_after_exact_requested_completed_instruction_steps(self):
        framebuffer = self.directory / "valid-steps.ppm"
        result = self.run_trace({"ASTRA_GS_WATCH": "0",
                                 "ASTRA_GS_FRAME_STEPS": "1,3,8"},
                                framebuffer=framebuffer)
        self.assertEqual(result.returncode, 1, result.stderr)
        steps = re.findall(r"^gs_frame_step step=(\d+) ee_cycle=(\d+) "
                           r"next_ee_pc=(\w+) scope=active-draw-target-not-scanout "
                           r"FRAME=(\w+) PMODE=(\w+) DISPFB1=(\w+) DISPFB2=(\w+)",
                           result.stdout, re.MULTILINE)
        self.assertEqual([(int(step), int(cycle), pc) for step, cycle, pc, *_ in steps],
                         [(1, 1, "BFC00004"), (3, 3, "BFC0000C"),
                          (8, 8, "BFC00020")])
        self.assertIn("gs_frame_steps captured=3 requested=3", result.stdout)
        self.assertEqual(re.findall(r"^gs_pixel_sample step=(\d+)", result.stdout,
                                    re.MULTILINE), ["1", "3", "8"])
        self.assertEqual(re.findall(
            r"^gs_frame_step_display step=(\d+) skipped=pmode-not-single-circuit "
            r"PMODE=0000000000000000 scope=logical-linear-not-scanout$",
            result.stdout, re.MULTILINE), ["1", "3", "8"])
        self.assertEqual(list(self.directory.glob("valid-steps*.linear-display.ppm")), [])
        expected_ppm = b"P6\n160 112\n255\n" + bytes(160 * 112 * 3)
        self.assertEqual(framebuffer.read_bytes(), expected_ppm)
        paths = sorted(self.directory.glob("valid-steps.ppm.step-*.ppm"))
        self.assertEqual([path.name for path in paths],
                         ["valid-steps.ppm.step-1.ppm", "valid-steps.ppm.step-3.ppm",
                          "valid-steps.ppm.step-8.ppm"])
        for path in paths:
            self.assertEqual(path.read_bytes(), expected_ppm)

    def test_full_step_capacity_and_early_stop_report_actual_capture_count(self):
        framebuffer = self.directory / "sixteen-steps.ppm"
        result = self.run_trace({"ASTRA_GS_FRAME_STEPS":
                                 ",".join(str(index) for index in range(1, 17))},
                                max_steps=16, framebuffer=framebuffer)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("gs_frame_steps captured=16 requested=16", result.stdout)
        self.assertEqual(len(list(self.directory.glob("sixteen-steps.ppm.step-*.ppm"))),
                         16)
        framebuffer = self.directory / "early-stop.ppm"
        result = self.run_trace({"ASTRA_GS_FRAME_STEPS": "1,3,8"},
                                framebuffer=framebuffer, stop_pc=0xBFC00008)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("gs_frame_steps captured=1 requested=3", result.stdout)
        self.assertTrue(Path(str(framebuffer) + ".step-1.ppm").is_file())
        self.assertFalse(Path(str(framebuffer) + ".step-3.ppm").exists())
        self.assertFalse(Path(str(framebuffer) + ".step-8.ppm").exists())

    @staticmethod
    def display_register_program(pmode, dispfb, circuit):
        # Owned EE instructions: LUI t0,0x1200; then LUI/ORI t1,value and
        # SW t1,offset(t0) for the privileged PMODE and selected DISPFB halves.
        words = [0x3C081200]
        offset = 0x70 if circuit == 1 else 0x90
        for address, value in ((0, pmode), (offset, dispfb & 0xFFFFFFFF),
                               (offset + 4, dispfb >> 32)):
            words.extend((0x3C090000 | ((value >> 16) & 0xFFFF),
                          0x35290000 | (value & 0xFFFF), 0xAD090000 | address))
        return words

    def test_supported_single_display_circuit_captures_decoded_offsets(self):
        for circuit, psm in ((1, 0), (2, 1)):
            with self.subTest(circuit=circuit, psm=psm):
                framebuffer = self.directory / f"display-{circuit}.ppm"
                fb = 3 | (1 << 9) | (psm << 15) | (5 << 32) | (7 << 43)
                program = self.display_register_program(1 << (circuit - 1), fb,
                                                         circuit)
                result = self.run_trace({"ASTRA_GS_FRAME_STEPS": "10"},
                                        max_steps=10, framebuffer=framebuffer,
                                        words=program)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f"gs_frame_step_display step=10 circuit={circuit} "
                              f"DISPFB={fb:016X} base=00006000 width=64 dbx=5 dby=7",
                              result.stdout)
                self.assertNotIn("gs_frame_step_display step=10 skipped=", result.stdout)
                display_path = Path(str(framebuffer) +
                                    ".step-10.ppm.linear-display.ppm")
                expected = b"P6\n160 112\n255\n" + bytes(160 * 112 * 3)
                self.assertEqual(display_path.read_bytes(), expected)

    def test_unsupported_or_multiple_display_circuits_skip_without_failure(self):
        cases = ((1, 0, "unsupported-display-format"),
                 (1, (1 << 9) | (2 << 15), "unsupported-display-format"),
                 (3, 1 << 9, "pmode-not-single-circuit"))
        for index, (pmode, fb, reason) in enumerate(cases):
            with self.subTest(pmode=pmode, fb=fb):
                framebuffer = self.directory / f"skipped-display-{index}.ppm"
                result = self.run_trace({"ASTRA_GS_FRAME_STEPS": "10"},
                                        max_steps=10, framebuffer=framebuffer,
                                        words=self.display_register_program(pmode, fb, 1))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f"gs_frame_step_display step=10 skipped={reason}",
                              result.stdout)
                self.assertTrue(Path(str(framebuffer) + ".step-10.ppm").is_file())
                self.assertFalse(Path(str(framebuffer) +
                                      ".step-10.ppm.linear-display.ppm").exists())


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: test_bios_trace_watch.py PS2BIOS_TRACE_BINARY")
    TRACE_BINARY = Path(sys.argv.pop(1)).resolve()
    unittest.main()
