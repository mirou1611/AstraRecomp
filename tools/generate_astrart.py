#!/usr/bin/env python3
"""Generate a small, deterministic AstraRT C++ package from PS2 ELF files.

This Phase-1 backend intentionally supports only the instructions covered by the
redistributable differential corpora. Unsupported instructions become explicit
interpreter exits rather than silently receiving guessed semantics.
"""

import argparse
import hashlib
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Set, Tuple


@dataclass
class Function:
    start: int
    end: int = 0
    direct_fallthrough: bool = False


def u32(blob: bytes, offset: int) -> int:
    return struct.unpack_from("<I", blob, offset)[0]


def read_elf(path: Path) -> Tuple[int, Dict[int, int], bytes]:
    blob = path.read_bytes()
    if len(blob) < 52 or blob[:7] != b"\x7fELF\x01\x01\x01":
        raise ValueError(f"{path}: expected little-endian ELF32")
    if struct.unpack_from("<H", blob, 18)[0] != 8:
        raise ValueError(f"{path}: expected MIPS machine type")
    entry, phoff = struct.unpack_from("<II", blob, 24)
    phentsize, phnum = struct.unpack_from("<HH", blob, 42)
    words: Dict[int, int] = {}
    for index in range(phnum):
        at = phoff + index * phentsize
        if at + 32 > len(blob):
            raise ValueError(f"{path}: truncated program header")
        kind, offset, vaddr, _, filesz, _, flags, _ = struct.unpack_from(
            "<IIIIIIII", blob, at
        )
        if kind != 1 or not (flags & 1):
            continue
        if offset + filesz > len(blob) or filesz % 4:
            raise ValueError(f"{path}: invalid executable segment")
        for delta in range(0, filesz, 4):
            address = vaddr + delta
            if address in words:
                raise ValueError(f"{path}: overlapping code at 0x{address:08x}")
            words[address] = u32(blob, offset + delta)
    if entry not in words:
        raise ValueError(f"{path}: entry point is not executable")
    return entry, words, blob


def opcode(word: int) -> int:
    return word >> 26


def jump_target(pc: int, word: int) -> int:
    return ((pc + 4) & 0xF0000000) | ((word & 0x03FFFFFF) << 2)


def is_jr(word: int) -> bool:
    return opcode(word) == 0 and (word & 0x3F) == 8


def is_break(word: int) -> bool:
    return opcode(word) == 0 and (word & 0x3F) == 13


def is_supported_data(word: int) -> bool:
    op = opcode(word)
    function = word & 0x3F
    return (
        word == 0
        or op in (0x09, 0x0C, 0x0D, 0x0F, 0x23, 0x2B)
        or (op == 0 and function in (0x21, 0x24, 0x2B))
    )


def is_supported_delay(word: int) -> bool:
    return is_supported_data(word)


def branch_target(pc: int, word: int) -> int:
    return (pc + 4 + (sx16(word) << 2)) & 0xFFFFFFFF


def discover(entries: Iterable[int], words: Dict[int, int]) -> List[Function]:
    leaders: Set[int] = set(entries)
    pending = list(entries)
    visited: Set[int] = set()

    def schedule(address: int) -> None:
        if address in words and address not in leaders:
            leaders.add(address)
            pending.append(address)

    while pending:
        pc = pending.pop(0)
        while pc in words and pc not in visited:
            visited.add(pc)
            word = words[pc]
            op = opcode(word)
            if op in (0x02, 0x03, 0x04, 0x05) or is_jr(word):
                if pc + 4 not in words:
                    raise ValueError(f"control transfer at 0x{pc:08x} has no delay slot")
                if pc + 4 in leaders:
                    raise ValueError(f"delay slot at 0x{pc + 4:08x} is also an entry")
                visited.add(pc + 4)
                if op in (0x02, 0x03):
                    schedule(jump_target(pc, word))
                elif op in (0x04, 0x05):
                    schedule(branch_target(pc, word))
                if op in (0x03, 0x04, 0x05):
                    schedule(pc + 8)
                break
            if is_break(word) or not is_supported_data(word):
                break
            pc += 4

    blocks: List[Function] = []
    for start in sorted(leaders):
        if start not in words or start not in visited:
            continue
        pc = start
        block = Function(start)
        while pc in words:
            if pc != start and pc in leaders:
                block.end = pc
                block.direct_fallthrough = True
                break
            word = words[pc]
            op = opcode(word)
            if op in (0x02, 0x03, 0x04, 0x05) or is_jr(word):
                block.end = pc + 8
                break
            if is_break(word) or not is_supported_data(word):
                block.end = pc + 4
                break
            pc += 4
        if not block.end:
            block.end = pc
        blocks.append(block)
    return blocks


def sx16(word: int) -> int:
    value = word & 0xFFFF
    return value - 0x10000 if value & 0x8000 else value


def emit_data(lines: List[str], pc: int, word: int, indent: str = "  ") -> None:
    op = opcode(word)
    rs = (word >> 21) & 31
    rt = (word >> 16) & 31
    rd = (word >> 11) & 31
    lines.append(f"{indent}state.pc = 0x{pc:08x}u;")
    if word == 0:
        lines.append(f"{indent}++executed;")
        return
    if op == 0x09:  # ADDIU
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = sign_extend_32(static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({sx16(word)}));"
            )
    elif op == 0 and (word & 0x3F) == 0x21:  # ADDU
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = sign_extend_32(static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>(state.gpr[{rt}]));"
            )
    elif op == 0 and (word & 0x3F) == 0x24:  # AND
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = state.gpr[{rs}] & state.gpr[{rt}];"
            )
    elif op == 0 and (word & 0x3F) == 0x2B:  # SLTU
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = state.gpr[{rs}] < state.gpr[{rt}] ? 1u : 0u;"
            )
    elif op == 0x0C:  # ANDI
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = state.gpr[{rs}] & 0x{word & 0xFFFF:04x}u;"
            )
    elif op == 0x0D:  # ORI
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = state.gpr[{rs}] | 0x{word & 0xFFFF:04x}u;"
            )
    elif op == 0x0F:  # LUI
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = sign_extend_32(0x{(word & 0xFFFF) << 16:08x}u);"
            )
    elif op == 0x23:  # LW
        address = f"address_{pc:08x}"
        lines.append(
            f"{indent}const std::uint32_t {address} = static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({sx16(word)});"
        )
        lines.append(
            f"{indent}if (({address} & 3u) != 0u || !memory.valid({address}, 4u)) {{"
        )
        lines.append(f"{indent}  commit(memory, state, executed, fast);")
        lines.append(
            f"{indent}  return {{AotExitKind::Interpreter, StopReason::None, state.pc, executed}};"
        )
        lines.append(f"{indent}}}")
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = sign_extend_32(memory.read32({address}));"
            )
    elif op == 0x2B:  # SW
        address = f"address_{pc:08x}"
        lines.append(
            f"{indent}const std::uint32_t {address} = static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({sx16(word)});"
        )
        lines.append(
            f"{indent}if (({address} & 3u) != 0u || !memory.valid({address}, 4u)) {{"
        )
        lines.append(f"{indent}  commit(memory, state, executed, fast);")
        lines.append(
            f"{indent}  return {{AotExitKind::Interpreter, StopReason::None, state.pc, executed}};"
        )
        lines.append(f"{indent}}}")
        lines.append(
            f"{indent}memory.write32({address}, static_cast<std::uint32_t>(state.gpr[{rt}]));"
        )
    else:
        raise ValueError(f"internal: unsupported data instruction 0x{word:08x}")
    lines.append(f"{indent}++executed;")
    lines.append(f"{indent}++fast;")


def emit_function(function: Function, words: Dict[int, int]) -> List[str]:
    name = f"generated_{function.start:08x}"
    lines = [f"AotExit {name}(Memory& memory, CpuState& state) {{", "  std::uint32_t executed = 0;", "  std::uint32_t fast = 0;"]
    lines.append("  switch (state.pc) {")
    lines.append(f"  case 0x{function.start:08x}u: goto entry_{function.start:08x};")
    lines.append("  default: return {AotExitKind::Interpreter, StopReason::None, state.pc, 0u};")
    lines.append("  }")

    pc = function.start
    needs_fallthrough = True
    while pc < function.end:
        if pc == function.start:
            lines.append(f"entry_{pc:08x}:")
        word = words[pc]
        op = opcode(word)
        if op == 0x03:  # JAL
            delay = words[pc + 4]
            if not is_supported_delay(delay):
                lines.extend([
                    f"  state.pc = 0x{pc:08x}u;",
                    "  commit(memory, state, executed, fast);",
                    "  return {AotExitKind::Interpreter, StopReason::None, state.pc, executed};",
                ])
                needs_fallthrough = False
                break
            lines.append(f"  state.pc = 0x{pc:08x}u;")
            lines.append(f"  state.gpr[31] = sign_extend_32(0x{pc + 8:08x}u);")
            lines.append("  ++executed;")
            lines.append("  ++fast;")
            emit_data(lines, pc + 4, delay)
            target = jump_target(pc, word)
            lines.extend([
                f"  state.pc = 0x{target:08x}u;",
                "  commit(memory, state, executed, fast);",
                f"  return {{AotExitKind::Direct, StopReason::None, 0x{target:08x}u, executed}};",
            ])
            needs_fallthrough = False
            break
        if op == 0x02:  # J
            delay = words[pc + 4]
            if not is_supported_delay(delay):
                lines.extend([
                    f"  state.pc = 0x{pc:08x}u;",
                    "  commit(memory, state, executed, fast);",
                    "  return {AotExitKind::Interpreter, StopReason::None, state.pc, executed};",
                ])
                needs_fallthrough = False
                break
            lines.append(f"  state.pc = 0x{pc:08x}u;")
            lines.append("  ++executed;")
            lines.append("  ++fast;")
            emit_data(lines, pc + 4, delay)
            target = jump_target(pc, word)
            lines.extend([
                f"  state.pc = 0x{target:08x}u;",
                "  commit(memory, state, executed, fast);",
                f"  return {{AotExitKind::Direct, StopReason::None, 0x{target:08x}u, executed}};",
            ])
            needs_fallthrough = False
            break
        if op in (0x04, 0x05):  # BEQ/BNE
            delay = words[pc + 4]
            if not is_supported_delay(delay):
                lines.extend([
                    f"  state.pc = 0x{pc:08x}u;",
                    "  commit(memory, state, executed, fast);",
                    "  return {AotExitKind::Interpreter, StopReason::None, state.pc, executed};",
                ])
                needs_fallthrough = False
                break
            rs = (word >> 21) & 31
            rt = (word >> 16) & 31
            comparison = "==" if op == 0x04 else "!="
            taken = branch_target(pc, word)
            fallthrough = pc + 8
            lines.append(f"  state.pc = 0x{pc:08x}u;")
            lines.append(f"  const bool taken = state.gpr[{rs}] {comparison} state.gpr[{rt}];")
            lines.append("  ++executed;")
            lines.append("  ++fast;")
            emit_data(lines, pc + 4, delay)
            lines.extend([
                f"  const std::uint32_t target = taken ? 0x{taken:08x}u : 0x{fallthrough:08x}u;",
                "  state.pc = target;",
                "  commit(memory, state, executed, fast);",
                "  return {AotExitKind::Direct, StopReason::None, target, executed};",
            ])
            needs_fallthrough = False
            break
        if is_jr(word):
            delay = words[pc + 4]
            if not is_supported_delay(delay):
                lines.extend([
                    f"  state.pc = 0x{pc:08x}u;",
                    "  commit(memory, state, executed, fast);",
                    "  return {AotExitKind::Interpreter, StopReason::None, state.pc, executed};",
                ])
                needs_fallthrough = False
                break
            rs = (word >> 21) & 31
            lines.append(f"  state.pc = 0x{pc:08x}u;")
            lines.append(f"  const std::uint32_t target = static_cast<std::uint32_t>(state.gpr[{rs}]);")
            lines.append("  ++executed;")
            emit_data(lines, pc + 4, delay)
            lines.extend([
                "  state.pc = target;",
                "  commit(memory, state, executed, fast);",
                "  return {AotExitKind::Indirect, StopReason::None, target, executed};",
            ])
            needs_fallthrough = False
            break
        if is_break(word):
            lines.extend([
                f"  state.pc = 0x{pc:08x}u;",
                "  ++executed;",
                "  commit(memory, state, executed, fast);",
                "  return {AotExitKind::Stop, StopReason::Break, state.pc, executed};",
            ])
            needs_fallthrough = False
            break
        if not is_supported_data(word):
            lines.extend([
                f"  state.pc = 0x{pc:08x}u;",
                "  commit(memory, state, executed, fast);",
                "  return {AotExitKind::Interpreter, StopReason::None, state.pc, executed};",
            ])
            needs_fallthrough = False
            break
        emit_data(lines, pc, word)
        pc += 4
    if needs_fallthrough:
        exit_kind = "Direct" if function.direct_fallthrough else "Interpreter"
        lines.extend([
            f"  state.pc = 0x{function.end:08x}u;",
            "  commit(memory, state, executed, fast);",
            f"  return {{AotExitKind::{exit_kind}, StopReason::None, state.pc, executed}};",
        ])
    lines.append("}")
    return lines


def generate(paths: List[Path], package_name: str) -> str:
    all_words: Dict[int, int] = {}
    entries: List[int] = []
    digest = hashlib.sha256()
    for path in paths:
        entry, words, blob = read_elf(path)
        entries.append(entry)
        digest.update(struct.pack("<Q", len(blob)))
        digest.update(blob)
        overlap = set(all_words).intersection(words)
        if overlap:
            raise ValueError(f"{path}: overlaps another ELF at 0x{min(overlap):08x}")
        all_words.update(words)
    functions = discover(entries, all_words)
    ranges = [(function.start, function.end, function.start)
              for function in functions]
    guest_min = min(item[0] for item in ranges)
    guest_max = max(item[1] for item in ranges)

    lines = [
        "// Generated by tools/generate_astrart.py. Do not edit.",
        '#include "ps2vita/aot.hpp"',
        '#include "ps2vita/memory.hpp"',
        "",
        "namespace ps2vita {",
        "namespace {",
        "std::uint64_t sign_extend_32(std::uint32_t value) {",
        "  return static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(value)));",
        "}",
        "void commit(Memory& memory, CpuState& state, std::uint32_t executed, std::uint32_t fast) {",
        "  state.gpr[0] = 0;",
        "  state.gpr_hi[0] = 0;",
        "  state.cycles += executed;",
        "  state.cop0[9] += executed;",
        "  state.fast_path_instructions += fast;",
        "  memory.advance(executed);",
        "}",
        "",
    ]
    for function in functions:
        lines.extend(emit_function(function, all_words))
        lines.append("")
    lines.append("constexpr AotFunctionEntry kGeneratedEntries[] = {")
    for start, end, owner in ranges:
        lines.append(f"  {{0x{start:08x}u, 0x{end:08x}u, generated_{owner:08x}}},")
    lines.extend([
        "};",
        "constexpr AotPackage kGeneratedPackage = {",
        f'  "{package_name}",',
        "  kGeneratedEntries,",
        "  sizeof(kGeneratedEntries) / sizeof(kGeneratedEntries[0]),",
        "  kAotAbiVersion,",
        f"  0x{guest_min:08x}u,",
        f"  0x{guest_max:08x}u,",
        f'  "{digest.hexdigest()}",',
        "};",
        "} // namespace",
        "",
        "const AotPackage& generated_phase0_aot_package() { return kGeneratedPackage; }",
        "} // namespace ps2vita",
        "",
    ])
    return "\n".join(lines)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--package-name", default="phase0-generated-v1")
    parser.add_argument("elf", nargs="+", type=Path)
    args = parser.parse_args()
    result = generate(args.elf, args.package_name)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding="utf-8", newline="\n")
    print(f"generated {args.output} from {len(args.elf)} ELF input(s)")


if __name__ == "__main__":
    main()
