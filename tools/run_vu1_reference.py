#!/usr/bin/env python3
"""Capture or strictly parse a final VU1-memory reference from PCSX2.

Example (all runtime artifacts stay in ignored directories):
  python tools/run_vu1_reference.py capture build-release/first-vif.bin \
    build-release/first-vif-pcsx2-vumem.bin \
    --pcsx2 .tools/pcsx2-v2.8.2/pcsx2-qt.exe \
    --profile-template .tools/pcsx2-reference-data/PCSX2

The runner creates a unique profile, disables VU speed hacks, enables EE serial
logging, and terminates only its own tracked process on completion or timeout.
It never changes the template profile or the original firmware. The raw result
is exactly 16 KiB in little-endian word order. It proves final memory contents,
not the sequence of earlier XGKICK deliveries which may have been overwritten.
"""

import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import tempfile
import time
from pathlib import Path

from make_vu1_reference_elf import BEGIN_MARKER, END_MARKER, VU_MEMORY_SIZE, build

ROW_PATTERN = re.compile(r"VUMEM ([0-9A-Fa-f]{4})" + r" ([0-9A-Fa-f]{8})" * 4)
REFERENCE_SETTINGS = {
    "UI": {"SetupWizardIncomplete": "false", "ConfirmShutdown": "false",
           "PauseOnFocusLoss": "false", "StartPaused": "false"},
    "EmuCore": {"EnablePatches": "false", "EnableCheats": "false",
                "EnableWideScreenPatches": "false", "EnableNoInterlacingPatches": "false",
                "EnableGameFixes": "false", "SaveStateOnShutdown": "false"},
    "EmuCore/Speedhacks": {"vuFlagHack": "false", "vuThread": "false",
                           "vu1Instant": "false", "EECycleRate": "0", "EECycleSkip": "0"},
    "EmuCore/CPU": {"VU1.Roundmode": "3"},
    "EmuCore/GS": {"Renderer": "13", "DumpGSData": "false", "SaveRT": "false",
                   "SaveFrame": "false", "SaveInfo": "false", "SaveTexture": "false",
                   "EnableVideoCapture": "false", "VsyncEnable": "false"},
    "Logging": {"EnableFileLogging": "true", "EnableEEConsole": "true",
                "EnableTimestamps": "false", "EnableSystemConsole": "false"},
    "MemoryCards": {"Slot1_Enable": "false", "Slot2_Enable": "false"},
    "SPU2/Output": {"OutputMuted": "true"},
}


def parse_serial(text: str) -> bytes:
    """Reject incomplete, duplicate, malformed or unordered VUMEM records.

    PCSX2 may prefix console lines with timestamps/console categories. Only
    that prefix is ignored; every line containing VUMEM must fully match the
    marker or row grammar and participate in one complete ordered dump.
    """
    begun = ended = False
    output = bytearray()
    for line_number, line in enumerate(text.splitlines(), 1):
        _, token, suffix = line.partition("VUMEM")
        if not token:
            continue
        record = token + suffix
        if record == BEGIN_MARKER:
            if begun or ended:
                raise ValueError(f"line {line_number}: duplicate begin marker")
            begun = True
        elif record == END_MARKER:
            if not begun or ended:
                raise ValueError(f"line {line_number}: unexpected end marker")
            if len(output) != VU_MEMORY_SIZE:
                raise ValueError(f"line {line_number}: end at {len(output)} bytes, expected {VU_MEMORY_SIZE}")
            ended = True
        else:
            match = ROW_PATTERN.fullmatch(record)
            if not match:
                raise ValueError(f"line {line_number}: malformed VUMEM record")
            if not begun or ended:
                raise ValueError(f"line {line_number}: row outside dump markers")
            address = int(match[1], 16)
            if address != len(output) or address >= VU_MEMORY_SIZE:
                raise ValueError(f"line {line_number}: expected address {len(output):04X}, got {address:04X}")
            output.extend(struct.pack("<IIII", *(int(word, 16) for word in match.groups()[1:])))
    if not begun or not ended or len(output) != VU_MEMORY_SIZE:
        raise ValueError("missing complete VUMEM begin/end markers or memory rows")
    return bytes(output)


def select_prefix(packet: bytes, boundary: int | None) -> bytes:
    if boundary is None:
        return packet
    if not 0 < boundary <= len(packet) or boundary % 4:
        raise ValueError("prefix boundary must be a positive aligned word offset within the packet")
    selected = packet[:boundary]
    return selected + bytes((-len(selected)) % 16)


def compare_memory(reference: bytes, actual: bytes) -> dict:
    if len(reference) != VU_MEMORY_SIZE or len(actual) != VU_MEMORY_SIZE:
        raise ValueError("both VU1 memory dumps must be exactly 16384 bytes")
    different_words = 0
    different_qwords = 0
    examples = []
    for address in range(0, VU_MEMORY_SIZE, 16):
        expected = struct.unpack_from("<IIII", reference, address)
        observed = struct.unpack_from("<IIII", actual, address)
        if expected == observed:
            continue
        different_words += sum(a != b for a, b in zip(expected, observed))
        different_qwords += 1
        if len(examples) < 32:
            examples.append({"address": f"{address:04X}",
                             "reference": [f"{word:08X}" for word in expected],
                             "actual": [f"{word:08X}" for word in observed]})
    return {"different_words": different_words, "total_words": VU_MEMORY_SIZE // 4,
            "different_qwords": different_qwords, "total_qwords": VU_MEMORY_SIZE // 16,
            "examples": examples, "examples_truncated": different_qwords > len(examples)}


def make_profile(template: Path, run_directory: Path, vu1_interpreter=False) -> Path:
    profile = run_directory / "PCSX2"
    ini_directory = profile / "inis"
    ini_directory.mkdir(parents=True)
    config = configparser.ConfigParser(interpolation=None, strict=False)
    config.optionxform = str
    ini_source = template / "inis" / "PCSX2.ini"
    with ini_source.open(encoding="utf-8-sig") as source:
        config.read_file(source)
    bios_name = config.get("Filenames", "BIOS")
    bios_folder = Path(config.get("Folders", "Bios", fallback="bios"))
    if not bios_folder.is_absolute():
        bios_folder = template / bios_folder
    firmware = (bios_folder / bios_name).resolve(strict=True)
    task_bios = profile / "bios"
    task_bios.mkdir()
    shutil.copy2(firmware, task_bios / firmware.name)
    # PCSX2 can generate firmware sidecars. Existing sidecars are copied into
    # this unique profile so all firmware-related writes remain local to it.
    for extension in (".mec", ".nvm"):
        sidecar = firmware.with_suffix(extension)
        if sidecar.is_file():
            shutil.copy2(sidecar, task_bios / sidecar.name)
    for section, settings in REFERENCE_SETTINGS.items():
        if not config.has_section(section):
            config.add_section(section)
        for key, value in settings.items():
            config.set(section, key, value)
    if not config.has_section("EmuCore/CPU/Recompiler"):
        config.add_section("EmuCore/CPU/Recompiler")
    config.set("EmuCore/CPU/Recompiler", "EnableVU1", "false" if vu1_interpreter else "true")
    # Never inherit absolute output locations from the template.
    for name, folder in {
        "Bios": "bios", "Snapshots": "snaps", "Savestates": "sstates",
        "MemoryCards": "memcards", "Logs": "logs", "Cheats": "cheats",
        "Patches": "patches", "UserResources": "resources", "Cache": "cache",
        "Textures": "textures", "InputProfiles": "inputprofiles", "Videos": "videos",
        "Covers": "covers", "GameSettings": "gamesettings",
        "DebuggerLayouts": "debuggerlayouts", "DebuggerSettings": "debuggersettings",
    }.items():
        config.set("Folders", name, folder)
        (profile / folder).mkdir(exist_ok=True)
    dumps = profile / "unused-gs-dumps"
    dumps.mkdir()
    for key in ("HWDumpDirectory", "SWDumpDirectory"):
        config.set("EmuCore/GS", key, dumps.as_posix())
    with (ini_directory / "PCSX2.ini").open("w", encoding="utf-8", newline="\n") as target:
        config.write(target)
    return profile


def capture(args) -> dict:
    emulator = args.pcsx2.resolve(strict=True)
    template = args.profile_template.resolve(strict=True)
    packet = args.packet.read_bytes()
    original_size = len(packet)
    packet = select_prefix(packet, args.prefix_bytes)
    image = build(packet, reset_vu1=not args.no_reset_vu1)
    work = args.work_dir.resolve()
    work.mkdir(parents=True, exist_ok=True)
    run_directory = Path(tempfile.mkdtemp(prefix="run-", dir=work))
    selected_packet = run_directory / "selected-vif.bin"
    selected_packet.write_bytes(packet)
    make_profile(template, run_directory, args.vu1_interpreter)
    elf = run_directory / "vu1-reference.elf"
    elf.write_bytes(image)
    log = run_directory / "pcsx2.log"
    arguments = [str(emulator), "-nogui", "-batch", "-nofullscreen",
                 "-datapath", str(run_directory), "-logfile", str(log),
                 "-elf", str(elf)]
    startupinfo = None
    creationflags = 0
    if os.name == "nt":
        startupinfo = subprocess.STARTUPINFO()
        startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        startupinfo.wShowWindow = subprocess.SW_HIDE
        creationflags = subprocess.CREATE_NO_WINDOW
    process = subprocess.Popen(arguments, cwd=emulator.parent, stdin=subprocess.DEVNULL,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                               startupinfo=startupinfo, creationflags=creationflags)
    started = time.monotonic()
    memory = None
    try:
        while time.monotonic() - started < args.timeout:
            if log.is_file():
                serial = log.read_text(encoding="utf-8", errors="replace")
                if END_MARKER in serial:
                    memory = parse_serial(serial)
                    break
            if process.poll() is not None:
                raise RuntimeError(f"PCSX2 exited with code {process.returncode}; inspect {log}")
            time.sleep(0.1)
        if memory is None:
            raise TimeoutError(f"no complete VU1 dump within {args.timeout:g}s; inspect {log}")
    finally:
        # The Popen object identifies only the child we created; never enumerate
        # or kill PCSX2 instances belonging to the user or another task.
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait(timeout=3)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(memory)
    metadata = {
        "emulator": str(emulator), "profile": str(run_directory / "PCSX2"),
        "log": str(log), "elf": str(elf), "elapsed_seconds": round(time.monotonic() - started, 3),
        "packet_bytes": len(packet), "packet_sha256": hashlib.sha256(packet).hexdigest(),
        "original_packet_bytes": original_size, "prefix_bytes": args.prefix_bytes,
        "selected_packet": str(selected_packet),
        "elf_sha256": hashlib.sha256(image).hexdigest(),
        "memory_bytes": len(memory), "memory_sha256": hashlib.sha256(memory).hexdigest(),
        "reset_vu1": not args.no_reset_vu1, "reference_settings": REFERENCE_SETTINGS,
        "zero_vu1_register_prelude": not args.no_reset_vu1,
        "zero_vu1_data_memory": True,
        "vu1_execution_mode": "interpreter" if args.vu1_interpreter else "microVU",
        "scope": "final VU1 data memory only; historical XGKICK deliveries are not reconstructed",
    }
    args.output.with_suffix(args.output.suffix + ".json").write_text(
        json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    return metadata


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    run = commands.add_parser("capture", help="run a bounded isolated PCSX2 reference")
    run.add_argument("packet", type=Path)
    run.add_argument("output", type=Path)
    run.add_argument("--pcsx2", type=Path, required=True)
    run.add_argument("--profile-template", type=Path, required=True,
                     help="existing PCSX2 data root containing inis/PCSX2.ini and a selected BIOS")
    run.add_argument("--work-dir", type=Path, default=Path(".tools/vu1-reference"))
    run.add_argument("--timeout", type=float, default=15.0)
    run.add_argument("--no-reset-vu1", action="store_true")
    run.add_argument("--vu1-interpreter", action="store_true",
                     help="cross-check PCSX2's VU1 interpreter instead of microVU")
    run.add_argument("--prefix-bytes", type=lambda text: int(text, 0),
                     help="known complete VIF command boundary; pad selected prefix with NOPs to qword alignment")
    parse = commands.add_parser("parse", help="strictly parse an existing serial log")
    parse.add_argument("log", type=Path)
    parse.add_argument("output", type=Path)
    compare = commands.add_parser("compare", help="compare two complete raw VU1 data-memory dumps")
    compare.add_argument("reference", type=Path)
    compare.add_argument("actual", type=Path)
    args = parser.parse_args()
    if args.command != "compare" and args.output.exists():
        parser.error(f"output already exists: {args.output}; choose a fresh filename")
    try:
        if args.command == "capture":
            if not 0 < args.timeout <= 60:
                parser.error("timeout must be greater than zero and at most 60 seconds")
            metadata = capture(args)
            print(json.dumps(metadata, indent=2))
        elif args.command == "parse":
            memory = parse_serial(args.log.read_text(encoding="utf-8", errors="replace"))
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_bytes(memory)
            print(f"wrote {len(memory)} bytes to {args.output}; "
                  f"sha256={hashlib.sha256(memory).hexdigest()}")
        else:
            result = compare_memory(args.reference.read_bytes(), args.actual.read_bytes())
            print(json.dumps(result, indent=2))
            return int(result["different_words"] != 0)
    except (OSError, ValueError, RuntimeError, TimeoutError) as error:
        parser.exit(1, f"{error}\n")


if __name__ == "__main__":
    raise SystemExit(main())
