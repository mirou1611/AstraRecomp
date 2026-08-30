# PS2Recomp Phase-0 integration

## Decision

AstraRecomp uses PS2Recomp as a host-side analysis and translation frontend, then
adapts emitted R5900 functions to the compact AstraRT Vita ABI. This proves the
pipeline early without committing the project to PS2Recomp's desktop-oriented
runtime and rendering stack.

Validated upstream revision:
`14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`.

## Adopt

- ELF, symbol, section, relocation, and function analysis
- R5900 instruction decoding and C++ semantic emission
- Direct-call binding and conservative indirect-call fallback concepts
- Fast guest-RAM access patterns
- MMI/VU0 helpers, HLE hooks, and IOP-profile ideas where useful

## Adapt at the AstraRT boundary

- Map generated `R5900Context` register accesses onto AstraRT state.
- Map runtime memory macros onto validated AstraRT memory helpers.
- Make exits such as BREAK, syscall, exception, and unresolved calls explicit.
- Preserve architectural PC checkpoints for faults and differential testing.
- Package translated functions by title/module/overlay instead of assuming one
  permanently resident desktop executable.

## Do not adopt as the final Vita architecture

- raylib, SDL, GL translation, or PVR as the long-term renderer
- a large desktop runtime in the Vita hot path
- context-store-heavy generated code as the final optimized backend

The intended end state adds SSA/liveness and ARM register allocation, SNR2-style
overlay packages, a title-specialized direct GXM renderer, and telemetry-driven
selection of translated versus interpreted regions.

## Proven Phase-0 path

`tools/make_smoke_elf.py` creates a three-instruction EE ELF:

```text
addiu v0, zero, 42
sw    v0, 0x2000(zero)
break
```

The PS2Recomp analyzer discovers one function at `0x1000`; the recompiler emits
one function with no decode failures, guest fallbacks, warnings, or errors. The
first automatic subset backend in `tools/generate_astrart.py` now emits the
equivalent compact AstraRT body directly from the corpus ELF. Host and Vita tests
compare it against the interpreter. Vita3K visibly reports:

```text
PS2R AOT T0: PASS   INTERP:42   NATIVE:42
```

The second redistributable corpus contains two functions. Its main function uses
JAL plus a delay slot, then resumes after a leaf returns via `jr $ra`. PS2Recomp
discovers two functions and one resumable entry. AstraRT exposes those through an
exact-entry table and explicit `Direct`/`Indirect` exits. Differential execution
on host and Vita reports:

```text
PS2R AOT T1 CALL/RET: PASS   NATIVE:42
```

The shared dispatcher follows direct and indirect exits up to a caller-provided
function budget. Exhausting that budget returns an `Interpreter` exit at the
current guest PC, so native code cannot monopolize the Vita thread indefinitely.
Translated entry metadata is owned by an `AotPackage`. Its table stays sorted and
uses allocation-free binary lookup, allowing later game modules or overlays to
swap packages without embedding a hash map or the host analyzer in AstraRT.
Packages are validated before native execution for ABI compatibility, source
fingerprint format, guest bounds, alignment, strict ordering, non-overlap, and
callable function pointers. Invalid packages yield directly to the interpreter.
Bounded dispatch performs this scan once before entering its native call loop.

The generated Phase-0 package currently supports ADDIU, ADDU, SLTU, AND, ANDI,
ORI, LUI, LW, SW, J, JAL, JR, BEQ, BNE, legal supported-data/NOP delay slots,
BREAK, and explicit interpreter exits.
Reachable branch targets and fallthroughs become sorted, non-overlapping exact
basic-block entries. Seven redistributable ELF inputs generate 15 entries with a
length-delimited aggregate SHA-256 fingerprint. Seeded host tests cover arithmetic
and memory values, the profile-guided scalar families with 64-bit edge inputs,
both paths of BEQ/BNE, converging J blocks, call/return, and an unsupported XORI
fallback at its exact PC. This small independent decoder is only the first
AstraRT backend slice; PS2Recomp remains the broader analysis frontend.
Generated LW/SW guards alignment and mapped ranges, yielding to the interpreter
at the faulting PC before access. Fault-capable memory operations are not emitted
inside delay slots until the runtime carries pending-branch exception context.

Reproduce both frontend runs with:

```bash
./scripts/run-ps2recomp-smoke.sh
./scripts/run-ps2recomp-chain.sh
```

BREAK is an explicit terminal boundary in AstraRT. The upstream runtime raises a
breakpoint exception and its emitted function contains a subsequent fallthrough
PC assignment; the adaptation deliberately retains PC `0x1008` at the terminal
boundary so it matches the interpreter contract.

## Licensing boundary

PS2Recomp is GPLv3 while original AstraRecomp code is currently MIT. Keep the
upstream checkout, binaries, and verbatim generated experiment outputs separated
until the distribution and licensing decision is explicit. Architectural facts
and independently written AstraRT interfaces may be documented here, but a public
combined release needs a deliberate license review.
