#!/usr/bin/env python3
"""Check owned arithmetic replays against executable PCSX2 memory observations.

The pinned hashes were observed with PCSX2 v2.8.2 in both microVU and VU1
interpreter modes, using round mode 3. These are executable reference results,
not physical PS2 verification or validation of the original hardware timing.
This gate runs only the supplied local replay executable; PCSX2 is not needed.
"""

import argparse
import hashlib
import subprocess
import tempfile
from pathlib import Path

from make_vu1_math_fixture import build


EXPECTED_MEMORY = {
    "finite": "744f60b9cf23deb979f79af5a66f746d56bae172d325b8bacc420e9cc1f4183a",
    "div-boundaries": "f350dbc5521eef9031fef04135822d31e938b73f7335a96c98ec6984b192de5b",
}
MEMORY_BYTES = 16384
REPLAY_TIMEOUT_SECONDS = 30
SCOPE = (
    "executable PCSX2 v2.8.2 both modes round 3 observations; "
    "not physical PS2 or original hardware timing"
)


def replay_summary(output: str | bytes | None) -> str:
    """Keep diagnostic summaries bounded; never print a full execution trace."""
    if isinstance(output, bytes):
        output = output.decode("utf-8", errors="replace")
    lines = (output or "").splitlines()
    summaries = [line for line in lines if line.startswith(
        ("accepted=", "vu_exit ", "vu_timing ", "store_trace "))]
    return "\n".join(line[:512] for line in summaries[-4:]) or "no replay summary"


def check_suite(replay: Path, directory: Path, suite: str, expected_hash: str) -> bool:
    packet, _ = build(suite)
    packet_path = directory / f"{suite}.bin"
    image_path = directory / f"{suite}.ppm"
    memory_path = directory / f"{suite}-memory.bin"
    packet_path.write_bytes(packet)
    try:
        completed = subprocess.run(
            [str(replay), str(packet_path), str(image_path), str(memory_path)],
            capture_output=True, text=True, timeout=REPLAY_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as error:
        print(f"FAIL {suite}: replay exceeded {REPLAY_TIMEOUT_SECONDS}s; expected_sha256={expected_hash}")
        print(replay_summary(error.stdout))
        return False
    except OSError as error:
        print(f"FAIL {suite}: cannot execute replay: {str(error)[:512]}")
        return False

    memory = memory_path.read_bytes() if memory_path.is_file() else b""
    observed_hash = hashlib.sha256(memory).hexdigest() if memory else "missing-or-empty"
    if completed.returncode != 0 or len(memory) != MEMORY_BYTES or observed_hash != expected_hash:
        print(f"FAIL {suite}: exit={completed.returncode} memory_bytes={len(memory)} "
              f"actual_sha256={observed_hash} expected_sha256={expected_hash}")
        print(replay_summary(completed.stdout))
        return False
    print(f"PASS {suite}: memory_bytes={len(memory)} sha256={observed_hash}")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("replay", type=Path, help="local ps2vif_replay executable")
    args = parser.parse_args()
    try:
        replay = args.replay.resolve(strict=True)
    except OSError as error:
        parser.error(f"cannot locate replay executable: {error}")
    if not replay.is_file():
        parser.error("replay executable must be a file")
    print(f"scope={SCOPE}")
    with tempfile.TemporaryDirectory(prefix="vu1-math-replay-") as temporary:
        directory = Path(temporary)
        results = [check_suite(replay, directory, suite, expected_hash)
                   for suite, expected_hash in EXPECTED_MEMORY.items()]
    return 0 if all(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
