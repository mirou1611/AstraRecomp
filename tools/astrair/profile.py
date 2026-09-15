"""Validated execution-census input for profile-guided AstraIR work."""

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import re
import struct
from typing import Any, Iterable, Tuple

from astrair.trace import BlockProfile, EdgeProfile


SCHEMA = "astrarecomp.execution-census"
SUPPORTED_VERSION = 3
FINGERPRINT_SCHEME = "sha256-length-prefixed-elf-set-v1"
_SHA256 = re.compile(r"[0-9a-f]{64}\Z")


class CensusError(ValueError):
    """Raised when census data cannot safely guide compilation."""


@dataclass(frozen=True)
class ExecutionProfile:
    fingerprint_sha256: str
    blocks: Tuple[BlockProfile, ...]
    edges: Tuple[EdgeProfile, ...]


def source_fingerprint(blobs: Iterable[bytes]) -> str:
    """Fingerprint an ordered source set using the AOT package contract."""
    digest = hashlib.sha256()
    count = 0
    for blob in blobs:
        digest.update(struct.pack("<Q", len(blob)))
        digest.update(blob)
        count += 1
    if count == 0:
        raise ValueError("at least one source blob is required")
    return digest.hexdigest()


def source_fingerprint_paths(paths: Iterable[Path]) -> str:
    return source_fingerprint(path.read_bytes() for path in paths)


def _object(value: Any, field: str) -> dict:
    if not isinstance(value, dict):
        raise CensusError(f"{field} must be an object")
    return value


def _array(value: Any, field: str) -> list:
    if not isinstance(value, list):
        raise CensusError(f"{field} must be an array")
    return value


def _count(value: Any, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise CensusError(f"{field} must be a non-negative integer")
    return value


def _pc(value: Any, field: str) -> int:
    if (not isinstance(value, str) or len(value) != 10 or
            not value.startswith("0x")):
        raise CensusError(f"{field} must be an 8-digit hexadecimal address")
    try:
        result = int(value[2:], 16)
    except ValueError as error:
        raise CensusError(f"{field} must be an 8-digit hexadecimal address") from error
    if result & 3:
        raise CensusError(f"{field} must be instruction-aligned")
    return result


def load_execution_profile(path: Path, expected_fingerprint: str) -> ExecutionProfile:
    """Load EE profile data only after exact source identity validation."""
    if not _SHA256.fullmatch(expected_fingerprint):
        raise ValueError("expected_fingerprint must be lowercase SHA-256")
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise CensusError(f"{path}: could not read execution census: {error}") from error
    root = _object(document, "census")
    if root.get("schema") != SCHEMA:
        raise CensusError(f"{path}: unsupported census schema")
    if root.get("version") != SUPPORTED_VERSION:
        raise CensusError(f"{path}: unsupported census version")

    source = _object(root.get("source"), "source")
    if source.get("kind") != "elf-set":
        raise CensusError(f"{path}: census source must be an ELF set")
    if source.get("fingerprint_scheme") != FINGERPRINT_SCHEME:
        raise CensusError(f"{path}: unsupported source fingerprint scheme")
    actual_fingerprint = source.get("fingerprint_sha256")
    if not isinstance(actual_fingerprint, str) or not _SHA256.fullmatch(actual_fingerprint):
        raise CensusError(f"{path}: malformed source fingerprint")
    if actual_fingerprint != expected_fingerprint:
        raise CensusError(
            f"{path}: source fingerprint mismatch: expected "
            f"{expected_fingerprint}, got {actual_fingerprint}"
        )

    ee = _object(root.get("ee"), "ee")
    blocks = []
    seen_blocks = set()
    for index, value in enumerate(_array(ee.get("blocks"), "ee.blocks")):
        item = _object(value, f"ee.blocks[{index}]")
        pc = _pc(item.get("pc"), f"ee.blocks[{index}].pc")
        entries = _count(item.get("entries"), f"ee.blocks[{index}].entries")
        if pc in seen_blocks:
            raise CensusError(f"{path}: duplicate EE block 0x{pc:08x}")
        seen_blocks.add(pc)
        blocks.append(BlockProfile(pc, entries))

    edges = []
    seen_edges = set()
    for index, value in enumerate(_array(ee.get("edges"), "ee.edges")):
        item = _object(value, f"ee.edges[{index}]")
        source_pc = _pc(item.get("source"), f"ee.edges[{index}].source")
        target_pc = _pc(item.get("target"), f"ee.edges[{index}].target")
        transitions = _count(
            item.get("transitions"), f"ee.edges[{index}].transitions"
        )
        pair = (source_pc, target_pc)
        if pair in seen_edges:
            raise CensusError(
                f"{path}: duplicate EE edge 0x{source_pc:08x} -> 0x{target_pc:08x}"
            )
        seen_edges.add(pair)
        edges.append(EdgeProfile(source_pc, target_pc, transitions))

    blocks.sort(key=lambda block: block.pc)
    edges.sort(key=lambda edge: (edge.source, edge.target))
    return ExecutionProfile(actual_fingerprint, tuple(blocks), tuple(edges))
