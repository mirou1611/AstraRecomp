# AstraRecomp — coworker review handoff

Date: 2026-08-29  
Project directory: `C:\Users\amir\Desktop\PS2 vita`  
Current milestone: PS2Recomp Phase-0 frontend integrated with a minimal Vita-native AstraRT dispatcher

## Executive summary

AstraRecomp is an experimental PC-assisted PlayStation 2 static-recompilation and
hybrid porting framework for PlayStation Vita. It is no longer intended to be only
a conventional full-system interpreter. The PC analyzes and translates selected
R5900/Emotion Engine code; the Vita runs that code through a compact runtime named
AstraRT. The existing interpreter remains the correctness oracle, BIOS bootstrap
path, and future fallback for untranslated code.

The project does **not yet show the PS2 BIOS intro**. The BIOS executes well into
hardware initialization, but complete IOP/SIF, interrupt, DMA, GIF, and GS behavior
is still required before the startup animation can appear.

The important result reached today is narrower but real: genuine PS2Recomp output
has been adapted to Vita-native ARM code, matched against the interpreter, packaged
as a VPK, and visibly validated in Vita3K.

## Verified results

### T0 — basic translated function

Guest program:

```mips
addiu v0, zero, 42
sw    v0, 0x2000(zero)
break
```

PS2Recomp discovered and emitted one function with:

```text
decode failures: 0
unhandled instructions: 0
guest fallbacks: 0
warnings: 0
errors: 0
```

AstraRT executes the adapted native function and compares its register, RAM, PC,
cycle, and stop state against the interpreter.

Visible result:

```text
PS2R AOT T0: PASS   INTERP:42   NATIVE:42
```

### T1 — translated call and return chain

The second redistributable ELF contains two guest functions and exercises:

- `JAL` direct call;
- a real MIPS delay slot;
- a translated leaf function;
- `jr $ra` indirect return;
- a resumable caller entry;
- guest RAM output and BREAK.

PS2Recomp discovered two functions and one additional resumable entry. It emitted
both functions with zero decode failures, fallbacks, warnings, or errors.

Visible result:

```text
PS2R AOT T1 CALL/RET: PASS   NATIVE:42
```

Latest Vita3K screenshot:

```text
C:\Users\amir\AppData\Local\Temp\astrarecomp-package-check.png
```

## Current AstraRT architecture

The relevant implementation is in:

- `include/ps2vita/aot.hpp`
- `src/core/aot.cpp`
- `tests/core_tests.cpp`
- `src/vita/main.cpp`

Translated functions use a small ABI:

```text
AotFunction(Memory&, CpuState&) -> AotExit
```

An exit explicitly describes one of four outcomes:

- `Direct`: continue at a statically known guest entry;
- `Indirect`: resolve a runtime target such as `$ra`;
- `Interpreter`: return to the portable EE path;
- `Stop`: BREAK, fault, syscall, or another terminal boundary.

Exact callable and resumable guest PCs live in a sorted `AotPackage` table.
AstraRT uses allocation-free binary search, so future title modules or overlays
can swap packages without embedding the host analyzer or a large hash map on Vita.

The shared dispatcher has a caller-provided function budget. If native execution
uses the entire budget, it yields an `Interpreter` exit at the current guest PC.
This prevents a bad package or endless native call cycle from monopolizing the
Vita thread.

## Existing emulator/bootstrap foundation

Before the AOT work, the project already implemented:

- 32 MiB EE RAM plus KSEG aliases;
- 4 MiB BIOS mapping and reset-vector execution;
- EE scratchpad and initial TLB behavior;
- portable R5900 interpreter with 128-bit GPR state;
- integer, branch/delay-slot, multiply/divide, COP0, partial COP1, and partial MMI support;
- EE/IOP-visible RAM and initial hardware-register windows;
- initial INTC, DMAC, timers, SBUS/SIF, and reset-ROM handshake behavior;
- an EE decoded-block cache and verified native RAM-clear fast path;
- a small software GS diagnostic renderer;
- Vita-native monitor UI and controls.

The BIOS currently runs through early initialization and reaches IOP/SIF boot
synchronization. This is real BIOS execution, not a prerecorded intro or fake UI.

## Why the PS2 intro is not visible yet

T0 and T1 prove translated EE execution and dispatch. They do not emulate all
hardware used by the BIOS startup sequence. The intro needs coordinated behavior
across at least:

1. IOP execution and reset-ROM services;
2. SIF communication between EE and IOP;
3. interrupt delivery and timing;
4. DMA channel behavior;
5. GIF packet transfer;
6. GS privileged registers and drawing/display state.

The current BIOS PC should be traced to identify its exact wait condition. The
next intro-oriented task is to satisfy that observed hardware dependency, then
capture the first GIF traffic and render it through the reference GS path.

## PS2Recomp relationship and license warning

Evaluated upstream project: `https://github.com/ran-j/PS2Recomp`  
Validated revision: `14b1e5cb39b4af7e6fc12f9a29fdc751efde49d7`

PS2Recomp is useful as the Phase-0 host frontend because it provides ELF/function
analysis, relocation handling, R5900 decoding, semantic C++ emission, direct-call
binding, indirect-call concepts, memory helpers, and HLE infrastructure.

Its full desktop/runtime stack is not the intended final Vita architecture. In
particular, AstraRecomp should not depend long-term on raylib, SDL, GL translation,
or a large desktop runtime. The intended Vita renderer is title-specialized and
eventually direct GXM.

PS2Recomp is GPLv3. Original AstraRecomp code is currently MIT. The upstream
checkout, binaries, and verbatim generated experiments must remain separated until
the combined-project distribution and licensing model receives explicit review.

## Build and reproduce

Host build and tests from WSL:

```bash
cd '/mnt/c/Users/amir/Desktop/PS2 vita'
cmake --build build-host -j4
ctest --test-dir build-host --output-on-failure
```

Reproduce both PS2Recomp frontend corpora:

```bash
./scripts/run-ps2recomp-smoke.sh
./scripts/run-ps2recomp-chain.sh
```

The scripts expect the isolated upstream build by default at:

```text
/home/mirou/astrarecomp-ps2recomp-spike-local/out-host-localdeps
```

Override it with the `PS2RECOMP_BUILD` environment variable if necessary.

Build the Vita VPK:

```bash
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
./scripts/build-vita.sh
```

Output:

```text
C:\Users\amir\Desktop\PS2 vita\build-vita\ps2vita.vpk
```

Latest verified VPK size: 132,064 bytes  
SHA-256: `A719E6C46DBD69EB7BF2C4F2F9994CFB8D52F63E29E5AAA770626C2E9F803BD3`

## Current ARM code-size baseline

From the Vita cross-compiled AOT object:

| Component | ARM `.text` size |
|---|---:|
| T0 translated body | 136 bytes |
| T1 caller/resume body | 292 bytes |
| T1 leaf body | 112 bytes |
| Binary entry lookup | 76 bytes |
| Single-function execute | 98 bytes |
| Bounded dispatcher | 156 bytes |
| Four-entry table | 48 bytes of read-only data |
| Package descriptor | 12 bytes of read-only data |

These are diagnostic corpus numbers, not estimates for a complete game.

## Known caveat in the upstream build

`ps2_analyzer` and `ps2_recomp` build and run successfully. The aggregate upstream
test target fails when `PS2X_BUILD_RUNTIME=OFF` because runtime tests are still
wired into that configuration and include runtime-only headers:

```text
Stubs/Unimplemented.h
ps2x/iop/iop_types.h
```

This appears to be an upstream CMake test-wiring issue. AstraRecomp's own host
differential test suite passes completely.

## Requested coworker review

Please focus review on these questions:

1. Is `AotExit` sufficient for direct calls, returns, faults, syscalls, and safe
   interpreter fallback without leaking PS2Recomp runtime types into AstraRT?
2. Should translated packages use exact-entry binary lookup, a generated page
   index, or a two-level table once function counts become large?
3. Are PC, cycle, delay-slot, exception, and zero-register invariants preserved at
   every native exit?
4. What is the best clean licensing boundary between the GPLv3 host frontend and
   the MIT-origin AstraRT code?
5. Which current BIOS wait loop should be attacked first to reach observable GIF
   traffic and the earliest possible startup image?
6. Which instruction subset should the automatic AstraRT backend support first?

## Recommended next implementation sequence

1. Build an automatic AstraRT backend for arithmetic, loads/stores, branches,
   JAL/JR, delay slots, and explicit exits.
2. Differentially fuzz generated basic blocks against the interpreter.
3. Add package validation: sorted entries, non-overlap, address ranges, ABI/version,
   and a source-ELF fingerprint.
4. Trace the current BIOS synchronization loop and identify its missing hardware
   event from actual MMIO reads/writes.
5. Implement that event and repeat until the first GIF packets appear.
6. Capture/replay those packets through the software GS renderer.
7. Only then specialize translated BIOS/title paths and begin a direct-GXM backend.

## Additional project documents

- `README.md`
- `docs/PS2RECOMP_INTEGRATION.md`
- `docs/ARCHITECTURE.md`
- `docs/ROADMAP.md`
- `docs/SESSION_2026-08-29.md`
- External design references in `C:\Users\amir\Desktop\PS2 vita2`

