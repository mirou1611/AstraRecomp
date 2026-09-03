#!/usr/bin/env python3
"""Generate a small, deterministic AstraRT C++ package from PS2 ELF files.

This Phase-1 backend intentionally supports only the instructions covered by the
redistributable differential corpora. Unsupported instructions become explicit
interpreter exits rather than silently receiving guessed semantics.
"""

import argparse
import re
import struct
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Set, Tuple

from astrair.builder import build_data_instruction
from astrair.analysis_effects import is_hard_barrier
from astrair.analysis_width import infer_block_results
from astrair.ir import Instruction, Op
from astrair.profile import load_execution_profile, source_fingerprint
from astrair.trace import BlockProfile, Trace, form_direct_traces, serialize_trace_plan


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
    return build_data_instruction(0, word) is not None


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


def emit_data(lines: List[str], pc: int, word: int, indent: str = "  ",
              instruction: Optional[Instruction] = None) -> None:
    instruction = instruction or build_data_instruction(pc, word)
    if instruction is None:
        raise ValueError(f"internal: unsupported data instruction 0x{word:08x}")
    kind = instruction.op
    rs = instruction.rs
    rt = instruction.rt
    rd = instruction.rd
    lines.append(f"{indent}state.pc = 0x{pc:08x}u;")
    if kind is Op.NOP:
        lines.append(f"{indent}++executed;")
        return
    if kind is Op.ADDIU:
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = sign_extend_32(static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({instruction.immediate}));"
            )
    elif kind is Op.ADDU:
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = sign_extend_32(static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>(state.gpr[{rt}]));"
            )
    elif kind is Op.AND:
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = state.gpr[{rs}] & state.gpr[{rt}];"
            )
    elif kind is Op.SLTU:
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = state.gpr[{rs}] < state.gpr[{rt}] ? 1u : 0u;"
            )
    elif kind in (Op.DADDU, Op.DSUBU):
        if rd:
            operator = "+" if kind is Op.DADDU else "-"
            lines.append(
                f"{indent}state.gpr[{rd}] = state.gpr[{rs}] {operator} state.gpr[{rt}];"
            )
    elif kind is Op.MULT:
        lines.append(
            f"{indent}const std::int64_t product_{pc:08x} = static_cast<std::int64_t>(static_cast<std::int32_t>(state.gpr[{rs}])) * static_cast<std::int32_t>(state.gpr[{rt}]);"
        )
        lines.append(
            f"{indent}state.lo = sign_extend_32(static_cast<std::uint32_t>(product_{pc:08x}));"
        )
        lines.append(
            f"{indent}state.hi = sign_extend_32(static_cast<std::uint32_t>(static_cast<std::uint64_t>(product_{pc:08x}) >> 32));"
        )
        if rd:
            lines.append(f"{indent}state.gpr[{rd}] = state.lo;")
    elif kind in (Op.PAND, Op.PXOR, Op.POR):
        operation = {
            Op.PAND: "&",
            Op.PXOR: "^",
            Op.POR: "|",
        }[kind]
        if rd:
            lines.append(
                f"{indent}state.gpr[{rd}] = state.gpr[{rs}] {operation} state.gpr[{rt}];"
            )
            lines.append(
                f"{indent}state.gpr_hi[{rd}] = state.gpr_hi[{rs}] {operation} state.gpr_hi[{rt}];"
            )
    elif kind is Op.ANDI:
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = state.gpr[{rs}] & 0x{word & 0xFFFF:04x}u;"
            )
    elif kind is Op.ORI:
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = state.gpr[{rs}] | 0x{word & 0xFFFF:04x}u;"
            )
    elif kind is Op.LUI:
        if rt:
            lines.append(
                f"{indent}state.gpr[{rt}] = sign_extend_32(0x{(word & 0xFFFF) << 16:08x}u);"
            )
    elif kind is Op.LW:
        address = f"address_{pc:08x}"
        lines.append(
            f"{indent}const std::uint32_t {address} = static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({instruction.immediate});"
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
    elif kind is Op.SW:
        address = f"address_{pc:08x}"
        lines.append(
            f"{indent}const std::uint32_t {address} = static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({instruction.immediate});"
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
    elif kind is Op.LD:
        address = f"address_{pc:08x}"
        lines.append(
            f"{indent}const std::uint32_t {address} = static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({instruction.immediate});"
        )
        lines.append(
            f"{indent}if (({address} & 7u) != 0u || !memory.valid({address}, 8u)) {{"
        )
        lines.append(f"{indent}  commit(memory, state, executed, fast);")
        lines.append(
            f"{indent}  return {{AotExitKind::Interpreter, StopReason::None, state.pc, executed}};"
        )
        lines.append(f"{indent}}}")
        if rt:
            lines.append(f"{indent}state.gpr[{rt}] = memory.read64({address});")
    elif kind is Op.SD:
        address = f"address_{pc:08x}"
        lines.append(
            f"{indent}const std::uint32_t {address} = static_cast<std::uint32_t>(state.gpr[{rs}]) + static_cast<std::uint32_t>({instruction.immediate});"
        )
        lines.append(
            f"{indent}if (({address} & 7u) != 0u || !memory.valid({address}, 8u)) {{"
        )
        lines.append(f"{indent}  commit(memory, state, executed, fast);")
        lines.append(
            f"{indent}  return {{AotExitKind::Interpreter, StopReason::None, state.pc, executed}};"
        )
        lines.append(f"{indent}}}")
        lines.append(f"{indent}memory.write64({address}, state.gpr[{rt}]);")
    lines.append(f"{indent}++executed;")
    lines.append(f"{indent}++fast;")


_GPR_REFERENCE = re.compile(r"state\.gpr\[(\d+)\]")
_GPR_WRITE = re.compile(r"^\s*state\.gpr\[(\d+)\]\s*=")


def defer_gpr_writeback(lines: List[str]) -> List[str]:
    """Cache referenced low GPRs and flush dirty locals on every commit path."""
    referenced = sorted({
        int(match.group(1))
        for line in lines for match in _GPR_REFERENCE.finditer(line)
        if match.group(1) != "0"
    })
    dirty = sorted({
        int(match.group(1))
        for line in lines
        for match in [_GPR_WRITE.match(line)]
        if match is not None and match.group(1) != "0"
    })
    if not referenced:
        return lines

    declarations = [
        f"  std::uint64_t gpr_{register} = state.gpr[{register}];"
        for register in referenced
    ]
    result = lines[:3] + declarations + lines[3:]
    transformed = []
    for line in result:
        if "commit(memory, state, executed, fast);" in line:
            indent = line[:len(line) - len(line.lstrip())]
            transformed.extend(
                f"{indent}state.gpr[{register}] = gpr_{register};"
                for register in dirty
            )
        if line in declarations:
            transformed.append(line)
            continue
        transformed.append(_GPR_REFERENCE.sub(
            lambda match: (f"gpr_{match.group(1)}"
                           if int(match.group(1)) in referenced
                           else match.group(0)),
            line,
        ))
    return transformed


def emit_function(function: Function, words: Dict[int, int],
                  deferred_writeback: bool) -> List[str]:
    name = f"generated_{function.start:08x}"
    lines = [f"AotExit {name}(Memory& memory, CpuState& state) {{", "  std::uint32_t executed = 0;", "  std::uint32_t fast = 0;"]
    lines.append("  switch (state.pc) {")
    lines.append(f"  case 0x{function.start:08x}u: goto entry_{function.start:08x};")
    lines.append("  default: return {AotExitKind::Interpreter, StopReason::None, state.pc, 0u};")
    lines.append("  }")

    linear_ir = []
    scan_pc = function.start
    while scan_pc < function.end:
        scan_word = words[scan_pc]
        if (opcode(scan_word) in (0x02, 0x03, 0x04, 0x05)
                or is_jr(scan_word) or is_break(scan_word)):
            break
        instruction = build_data_instruction(scan_pc, scan_word)
        if instruction is None:
            break
        linear_ir.append(instruction)
        scan_pc += 4
    refined_ir = {
        instruction.pc: instruction
        for instruction in infer_block_results(linear_ir)
    }

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
        emit_data(lines, pc, word, instruction=refined_ir.get(pc))
        pc += 4
    if needs_fallthrough:
        exit_kind = "Direct" if function.direct_fallthrough else "Interpreter"
        lines.extend([
            f"  state.pc = 0x{function.end:08x}u;",
            "  commit(memory, state, executed, fast);",
            f"  return {{AotExitKind::{exit_kind}, StopReason::None, state.pc, executed}};",
        ])
    lines.append("}")
    return defer_gpr_writeback(lines) if deferred_writeback else lines


def _validate_direct_traces(traces: Iterable[Trace], functions: List[Function],
                            words: Dict[int, int]) -> List[Trace]:
    traces = sorted(traces, key=lambda trace: trace.blocks[0] if trace.blocks else -1)
    known = {function.start for function in functions}
    direct_successors, hard_barriers = _static_trace_inputs(functions, words)
    owned = set()
    for trace in traces:
        if len(trace.blocks) < 2:
            raise ValueError("executable direct traces must contain at least two blocks")
        if trace.guards:
            raise ValueError("guarded traces are not executable yet")
        for block in trace.blocks:
            if block not in known:
                raise ValueError(f"trace block 0x{block:08x} is not a generated entry")
            if block in owned:
                raise ValueError(f"trace block 0x{block:08x} has multiple owners")
            owned.add(block)
            if block in hard_barriers:
                raise ValueError(f"executable trace contains hard barrier at 0x{block:08x}")
        for source, target in zip(trace.blocks, trace.blocks[1:]):
            if direct_successors.get(source) != frozenset({target}):
                raise ValueError(
                    f"trace edge 0x{source:08x} -> 0x{target:08x} is not uniquely direct"
                )
    return traces


def _emit_inline_direct_block(lines: List[str], function: Function,
                              target: int, words: Dict[int, int]) -> None:
    pc = function.start
    while pc < function.end:
        word = words[pc]
        op = opcode(word)
        if op in (0x02, 0x03):
            if jump_target(pc, word) != target:
                raise ValueError("inline direct block target changed")
            delay = words[pc + 4]
            if not is_supported_delay(delay):
                raise ValueError("inline direct block has unsupported delay slot")
            if op == 0x03:
                lines.append(f"  state.pc = 0x{pc:08x}u;")
                lines.append(f"  state.gpr[31] = sign_extend_32(0x{pc + 8:08x}u);")
            else:
                lines.append(f"  state.pc = 0x{pc:08x}u;")
            lines.append("  ++executed;")
            lines.append("  ++fast;")
            emit_data(lines, pc + 4, delay)
            return
        if is_jr(word) or is_break(word) or op in (0x04, 0x05):
            raise ValueError("inline trace block is not uniquely direct")
        instruction = build_data_instruction(pc, word)
        if instruction is None:
            raise ValueError("inline trace block contains unsupported instruction")
        emit_data(lines, pc, word, instruction=instruction)
        pc += 4
    if not function.direct_fallthrough or function.end != target:
        raise ValueError("inline trace block has no direct fallthrough")


def _emit_fused_trace(trace: Trace, functions: List[Function],
                      words: Dict[int, int], deferred_writeback: bool) -> List[str]:
    name = f"generated_trace_{trace.blocks[0]:08x}"
    by_start = {function.start: function for function in functions}
    planned_cycles = sum(
        (by_start[block].end - by_start[block].start) // 4
        for block in trace.blocks
    )
    lines = [
        f"AotExit {name}(Memory& memory, CpuState& state) {{",
        "  std::uint32_t executed = 0;",
        "  std::uint32_t fast = 0;",
        f"  if (memory.cycles_until_next_event() <= {planned_cycles}u)",
        f"    return generated_{trace.blocks[0]:08x}(memory, state);",
    ]
    for source, target in zip(trace.blocks, trace.blocks[1:]):
        _emit_inline_direct_block(lines, by_start[source], target, words)
    tail = emit_function(by_start[trace.blocks[-1]], words, False)
    if tail[3:7] != [
            "  switch (state.pc) {",
            f"  case 0x{trace.blocks[-1]:08x}u: goto entry_{trace.blocks[-1]:08x};",
            "  default: return {AotExitKind::Interpreter, StopReason::None, state.pc, 0u};",
            "  }"]:
        raise ValueError("internal: generated function prologue changed")
    if tail[7] != f"entry_{trace.blocks[-1]:08x}:":
        raise ValueError("internal: generated entry label changed")
    lines.extend(tail[8:-1])
    lines.append("}")
    return defer_gpr_writeback(lines) if deferred_writeback else lines


def generate(paths: List[Path], package_name: str,
             deferred_writeback: bool = False,
             direct_traces: Iterable[Trace] = ()) -> str:
    all_words: Dict[int, int] = {}
    entries: List[int] = []
    source_blobs = []
    for path in paths:
        entry, words, blob = read_elf(path)
        entries.append(entry)
        source_blobs.append(blob)
        overlap = set(all_words).intersection(words)
        if overlap:
            raise ValueError(f"{path}: overlaps another ELF at 0x{min(overlap):08x}")
        all_words.update(words)
    functions = discover(entries, all_words)
    direct_traces = _validate_direct_traces(direct_traces, functions, all_words)
    trace_entries = {trace.blocks[0] for trace in direct_traces}
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
        lines.extend(emit_function(function, all_words, deferred_writeback))
        lines.append("")
    for trace in direct_traces:
        lines.extend(_emit_fused_trace(
            trace, functions, all_words, deferred_writeback
        ))
        lines.append("")
    lines.append("constexpr AotFunctionEntry kGeneratedEntries[] = {")
    for start, end, owner in ranges:
        function_name = (f"generated_trace_{owner:08x}"
                         if start in trace_entries else f"generated_{owner:08x}")
        lines.append(f"  {{0x{start:08x}u, 0x{end:08x}u, {function_name}}},")
    lines.extend([
        "};",
        "constexpr AotPackage kGeneratedPackage = {",
        f'  "{package_name}",',
        "  kGeneratedEntries,",
        "  sizeof(kGeneratedEntries) / sizeof(kGeneratedEntries[0]),",
        "  kAotAbiVersion,",
        f"  0x{guest_min:08x}u,",
        f"  0x{guest_max:08x}u,",
        f'  "{source_fingerprint(source_blobs)}",',
        "};",
        "} // namespace",
        "",
        "const AotPackage& generated_phase0_aot_package() { return kGeneratedPackage; }",
        "} // namespace ps2vita",
        "",
    ])
    return "\n".join(lines)


def _static_trace_inputs(functions: List[Function], words: Dict[int, int]):
    direct_successors = {}
    hard_barriers = set()
    for function in functions:
        successors = set()
        if function.direct_fallthrough:
            successors.add(function.end)
        pc = function.start
        while pc < function.end:
            word = words[pc]
            instruction = build_data_instruction(pc, word)
            if instruction is not None and is_hard_barrier(instruction):
                hard_barriers.add(function.start)
            op = opcode(word)
            if op in (0x02, 0x03, 0x04, 0x05) or is_jr(word):
                delay = build_data_instruction(pc + 4, words[pc + 4])
                if delay is None or is_hard_barrier(delay):
                    hard_barriers.add(function.start)
            if op in (0x02, 0x03):
                successors.add(jump_target(pc, word))
                break
            if op in (0x04, 0x05):
                successors.update((branch_target(pc, word), pc + 8))
                break
            if is_jr(word) or is_break(word) or instruction is None:
                break
            pc += 4
        direct_successors[function.start] = frozenset(successors)
    return direct_successors, hard_barriers


def select_direct_traces(paths: List[Path], census_path: Path,
                         minimum_probability: float = 0.90,
                         maximum_blocks: int = 128) -> Tuple[List[Trace], str]:
    all_words: Dict[int, int] = {}
    entries = []
    blobs = []
    for path in paths:
        entry, words, blob = read_elf(path)
        overlap = set(all_words).intersection(words)
        if overlap:
            raise ValueError(f"{path}: overlaps another ELF at 0x{min(overlap):08x}")
        entries.append(entry)
        blobs.append(blob)
        all_words.update(words)
    fingerprint = source_fingerprint(blobs)
    profile = load_execution_profile(census_path, fingerprint)
    functions = discover(entries, all_words)
    direct_successors, hard_barriers = _static_trace_inputs(functions, all_words)
    known_blocks = {function.start for function in functions}
    blocks = (
        BlockProfile(block.pc, block.entries, block.pc in hard_barriers)
        for block in profile.blocks if block.pc in known_blocks
    )
    edges = (
        edge for edge in profile.edges
        if edge.source in known_blocks and edge.target in known_blocks
    )
    traces = form_direct_traces(
        blocks, edges, direct_successors, minimum_probability, maximum_blocks
    )
    return traces, fingerprint


def generate_trace_plan(paths: List[Path], census_path: Path,
                        minimum_probability: float = 0.90,
                        maximum_blocks: int = 128) -> str:
    """Build deterministic review metadata from a bound execution census."""
    traces, fingerprint = select_direct_traces(
        paths, census_path, minimum_probability, maximum_blocks
    )
    return serialize_trace_plan(traces, fingerprint)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--package-name", default="phase0-generated-v1")
    parser.add_argument("--defer-gpr-writeback", action="store_true")
    parser.add_argument("--execution-census", type=Path)
    parser.add_argument("--trace-plan-output", type=Path)
    parser.add_argument("--trace-minimum-probability", type=float, default=0.90)
    parser.add_argument("--trace-maximum-blocks", type=int, default=128)
    parser.add_argument("--enable-direct-traces", action="store_true")
    parser.add_argument("elf", nargs="+", type=Path)
    args = parser.parse_args()
    if bool(args.execution_census) != bool(args.trace_plan_output):
        parser.error("--execution-census and --trace-plan-output must be used together")
    traces = ()
    if args.enable_direct_traces:
        if not args.execution_census:
            parser.error("--enable-direct-traces requires --execution-census")
        traces, _ = select_direct_traces(
            args.elf, args.execution_census,
            args.trace_minimum_probability, args.trace_maximum_blocks,
        )
        traces = tuple(trace for trace in traces if len(trace.blocks) >= 2)
    result = generate(
        args.elf, args.package_name, args.defer_gpr_writeback, traces
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(result, encoding="utf-8", newline="\n")
    if args.execution_census:
        trace_plan = generate_trace_plan(
            args.elf, args.execution_census,
            args.trace_minimum_probability, args.trace_maximum_blocks,
        )
        args.trace_plan_output.parent.mkdir(parents=True, exist_ok=True)
        args.trace_plan_output.write_text(trace_plan, encoding="utf-8", newline="\n")
    print(f"generated {args.output} from {len(args.elf)} ELF input(s)")


if __name__ == "__main__":
    main()
