#!/usr/bin/env python3
"""Create a deterministic, fingerprint-bound census for the AOT oracle corpus."""

import argparse
import json
from pathlib import Path

from astrair.profile import FINGERPRINT_SCHEME, source_fingerprint_paths


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("elf", nargs="+", type=Path)
    args = parser.parse_args()
    document = {
        "schema": "astrarecomp.execution-census",
        "version": 3,
        "source": {
            "kind": "elf-set",
            "fingerprint_scheme": FINGERPRINT_SCHEME,
            "fingerprint_sha256": source_fingerprint_paths(args.elf),
        },
        "ee": {
            "blocks": [
                {"pc": "0x00003000", "entries": 100},
                {"pc": "0x00003020", "entries": 100},
            ],
            "edges": [
                {
                    "source": "0x00003000",
                    "target": "0x00003020",
                    "transitions": 100,
                }
            ],
        },
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(document, indent=2, sort_keys=True) + "\n",
        encoding="utf-8", newline="\n",
    )
    print(f"wrote fingerprint-bound AOT census to {args.output}")


if __name__ == "__main__":
    main()
