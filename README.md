<p align="center">
  <img src="docs/assets/astrarecomp-title-banner.png" alt="AstraRecomp title surrounded by crystalline code and luminous translation streams" width="100%">
</p>

<h1 align="center">AstraRecomp — PS2 on PS Vita</h1>

<p align="center">
  <strong>Exploring a native path from PlayStation 2 to PlayStation Vita.</strong><br>
  Experimental PC-assisted static recompilation, PS2 BIOS bring-up, and a Vita-native hybrid runtime.<br>
  Early research: no playable retail games, no confirmed startup intro, and no audio output yet.
</p>

<p align="center">
  <img alt="Status: experimental" src="https://img.shields.io/badge/status-experimental-875BFF?style=flat-square">
  <img alt="Language: C++17" src="https://img.shields.io/badge/C%2B%2B-17-35D9FF?style=flat-square&logo=cplusplus&logoColor=050711">
  <img alt="Target: PlayStation Vita" src="https://img.shields.io/badge/target-Vita-5271FF?style=flat-square">
  <a href="LICENSE"><img alt="License: MIT" src="https://img.shields.io/badge/license-MIT-18234A?style=flat-square"></a>
</p>

<p align="center">
  <a href="docs/ROADMAP.md">Roadmap</a> ·
  <a href="docs/ARCHITECTURE.md">Architecture</a> ·
  <a href="docs/PS2RECOMP_INTEGRATION.md">Recompiler boundary</a> ·
  <a href="docs/PERFORMANCE_VALIDATION.md">Performance gate</a> ·
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="docs/BRAND.md">Visual system</a>
</p>

AstraRecomp is an experimental PC-assisted PlayStation 2 static-recompilation and
hybrid porting framework for a homebrew-enabled PlayStation Vita. Heavy analysis
and translation happen on the PC; the Vita runs a small native runtime called
**AstraRT**. The existing Emotion Engine interpreter remains a correctness oracle,
BIOS/bootstrap path, and future cold-code fallback rather than the final fast path.

The goal is to investigate whether selected PS2 software can become practical on
Vita through **per-title analysis and native ARM code**, rather than assuming a
general-purpose desktop emulator can simply be moved to a handheld. The PC does
the expensive preparation; the Vita executes the resulting runtime and generated
code. This is a research direction, not a promise of universal compatibility or
full-speed PS2 games.

**Follow active development:**
[`research/spu2-dma-checkpoint`](https://github.com/mirou1611/AstraRecomp/tree/research/spu2-dma-checkpoint).
The status below describes that research branch; `main` is an earlier code
checkpoint with this updated project overview. For the latest implementation,
switch to the research branch before building.

The current milestone is intentionally small but real: PS2Recomp analyzes the
initial MIPS corpora, while an independently written subset backend now reads the
ELF code and automatically emits compact AstraRT C++. Differential tests execute
both interpreter and native paths. The monitor shows `PS2R AOT T0: PASS` when
register, memory, PC, cycle, and stop state agree. A second probe shows
`PS2R AOT T1 CALL/RET: PASS` after chaining generated blocks through a real
JAL/delay-slot/`jr $ra` sequence.

<p align="center">
  <img src="docs/assets/astrart-performance-gate.png" alt="Interpreter and native execution paths reconverging at an exact-state validation gate" width="100%">
</p>

A generated synthetic PS2 workload now exercises integer ALU, 64-bit math, MMI,
loads/stores, branches, calls/returns, memcpy-style loops, and multiply-heavy
matrix work. The Vita app compares it against the interpreter, records the median
of five samples in `benchmark.txt`, and shows `AOT PERF: PASS` when state agrees.
Vita3K is the first correctness gate; only a physical Vita result is a meaningful
performance decision.

It now executes a user-supplied retail BIOS through EE hardware initialization,
relocation into RAM, IOP reset-ROM startup, DECI2 startup, bidirectional SIF boot
traffic, and repeated `Restart Without Memory Clear` completion.
It does **not yet render a confirmed recognizable PS2 startup or run retail games**. Those require deeper IOP, DMA,
interrupt, GIF/GS, SPU2, and disc emulation described in the roadmap. The existing
core is real BIOS execution, not a renamed frontend or a fake compatibility screen.

## Project status

Latest verified research checkpoint: **September 5, 2026**. These are development
results, not a game-compatibility list or a measured completion percentage.

| Area | Current state |
| --- | --- |
| BIOS bootstrap | Executes through DECI2 startup, multi-tag SIF transfers, relocated EE code, and repeated no-clear restart completion |
| PC recompiler | Phase-0 analysis, a tested R5900-to-C++ subset, and a generated mixed-workload performance gate |
| Vita runtime | VitaSDK-only VPK, native monitor, ELF loading, stepping, and diagnostic framebuffer |
| Guest graphics | GIF/VIF1/VU1 path connected to a 160×112 software rasterizer; BIOS replay emits 5 triangles and 122 nonzero pixels, not a confirmed intro |
| VU1 | Tested instruction subset, microprogram upload, vector unpacking, XGKICK delivery, and partial MAC-flag latency; full timing remains incomplete |
| Audio / SPU2 | Sound RAM and tested DMA4/DMA7 transfer timing, status and interrupts; no sound decoding, voice mixing, or Vita speaker output yet |
| Retail games | **Not playable**—IOP devices, GIF/GS, VU, SPU2, media, and compatibility work remain |

### What the latest graphics milestone actually means

A deterministic host replay executes **248,800,000 EE steps**, with **998 VU1
instruction pairs**, **5 emitted triangles**, and **122/17,920 nonzero framebuffer
pixels**. Correct guest depth comparisons changed the previous fully black result.
The host test suite passes (2/2 CTest targets), and the Vita VPK cross-build passes.

This is evidence that part of the guest graphics pipeline reaches the rasterizer,
**not evidence of a recognizable boot animation, playable games, or acceptable
speed on physical Vita hardware**. One oversized XGKICK tag remains rejected.
Texture/fog rendering, GS depth-buffer formats/addressing, other pixel tests, and
VU timing still need work. See the
[saved investigation and replay details](https://github.com/mirou1611/AstraRecomp/blob/research/spu2-dma-checkpoint/docs/SESSION_2026-09-05.md).

### What comes next

- Capture and inspect the BIOS framebuffer and trace the rejected graphics packet.
- Improve the VU and GS behavior needed for a recognizable startup sequence.
- Build the missing SPU2 sound-generation and Vita audio-output path.
- Expand native-code coverage while checking it against the interpreter.
- Measure correctness and performance on a physical Vita before making speed claims.

PS2 and Vita developers can help with small reproducible tests, hardware reports,
instruction/device correctness, and graphics debugging. See
[Contributing](CONTRIBUTING.md). Please do not submit BIOS files or copyrighted
game assets; this repository does not include them.

## System shape

![Abstract streams of legacy machine code passing through a crystalline translation core into a compact runtime](docs/assets/astra-hero.png)

```text
PS2 ELF / user BIOS
        │
        ▼
 PC analysis + AOT emission ──────┐
        │                         │ exact-state fallback
        ▼                         ▼
 generated AstraRT C++      portable EE/IOP interpreter
        └──────────────┬──────────┘
                       ▼
             Vita-native AstraRT VPK
```

## Implemented foundation

<details>
<summary><strong>Expand the current implementation inventory</strong></summary>


- Vita-native VPK target using VitaSDK only (no runtime dependencies)
- PS2Recomp Phase-0 ELF analysis and R5900-to-C++ emission on the host
- Deterministic ELF-to-AstraRT generator for ADDIU/ADDU, LW/SW, J/JAL/JR,
  BEQ/BNE, 64-bit load/store and arithmetic, selected MMI/multiply operations,
  delay slots, BREAK, and exact interpreter fallback
- AstraRT AOT function boundary with seeded interpreter/native differential validation
- Exact-entry function table with direct-call, indirect-return, interpreter,
  and terminal exit contracts
- Strict ELF32, little-endian, MIPS executable loader
- 32 MiB EE RAM and KSEG0/KSEG1 aliases
- 16 KiB EE scratchpad and read-only 4 MiB BIOS mapping/reset vector
- 64 KiB EE MMIO window with initial INTC/DMAC and Timer 0 semantics
- EE-visible GS privileged-register aperture at `0x12000000`
- 2 MiB IOP RAM mirrored through the EE-visible 8 MiB IOP window
- Portable R3000A IOP interpreter with branch/load delays, exceptions, and
  reset-ROM cache-isolation behavior
- PS2-compatible null-bus behavior for uninstalled high memory
- Initial EE-visible IOP hardware register window at `0x1F801000`
- EE TLB translation with paired pages, variable page masks, and TLB instructions
- R5900 scratchpad TLB `S` mapping across its complete aligned 16 KiB aperture
- R5900 processor-internal control page used by early BIOS cache initialization
- Hardware-derived R5900 COP0/FPU reset identity and coprocessor-enable state
- COP0 Count driven by interpreted EE cycles for BIOS delay calibration
- EE/IOP SBUS flag semantics with guest-generated SIFINIT and BOOTEND events
- Cycle-scheduled SIF1 REF/REFE DMA from EE RAM into IOP RAM, including payload
  continuation across source tags, completion flags, and external-interrupt entry
- Cycle-scheduled SIF0 IOP-to-EE reply DMA with tagged destination delivery,
  quadword padding, independently terminating IOP/EE channels, and interrupt wakeup
- Dedicated R5900 level-one interrupt vector and boot-path PLZCW semantics
- Shared EE/IOP physical CDVD register window used by relocated BIOS code
- Master-cycle-driven 32-bit IOP Timer 5 with 16/32-bit register access,
  clock prescaling, pulsed-repeat target/overflow IRQs, and INTC bit 16 delivery
- Initial no-disc CDVD device state with N-READY/status registers and bounded
  OpenConfig/ReadConfig/WriteConfig/CloseConfig secondary-command transport
- Initial no-device SIO2 PIO state with reset/status registers, disconnected
  FIFO responses, transfer-start completion, and IOP interrupt 17
- Deterministic 59.94 Hz NTSC field phase with fractional-cycle correction and
  synchronized EE/IOP VBlank start and end interrupts
- HBlank-driven EE Timer 3 with rational scanline phase, 16-bit target/overflow
  behavior, write-one-to-clear reached flags, and INTC bit 12 delivery
- Absent development-board debug aperture with retail-style null-device behavior
- Portable EE interpreter foundation with 128-bit GPR storage
- Fixed-size decoded EE block cache with delay-slot boundaries, hotness tracking,
  and guest-RAM page generation invalidation
- Integer arithmetic, branches and delay slots, jumps, multiply/divide, COP0
  register moves, unaligned merge operations, and byte through quadword memory ops
- Initial MMI support (`PADDUW`, `DIV1/DIVU1`, `HI1/LO1` moves) using full
  128-bit GPR state and the EE's second accumulator pair
- Packed MMI accumulator save/restore and composition (`PMFHI`, `PMFLO`,
  `PMTHI`, `PMTLO`, `PCPYLD`, `PCPYUD`, and `PEXTLW`)
- R5900 shift-amount register state with `MFSA`/`MTSA` save and restore
- Scalar COP1 register moves, arithmetic, conversion, comparison, and branches
- Scalar COP1 memory transfers (`LWC1`/`SWC1`)
- Verified native semantic zero-fill acceleration for the BIOS's large RAM clear
- 160x112 software GS reference framebuffer with depth-tested points, lines, and triangles
- Research branch: GIF PACKED/REGLIST/IMAGE parsing, logical texture uploads,
  initial textured sprites, ADC draw suppression, and guest depth-test/write-mask state
- Research branch: VIF1 microprogram upload/unpack and a VU1 interpreter subset
  delivering XGKICK packets to the GIF frontend
- Research branch: 2 MiB SPU2 sound RAM with scheduled DMA4/DMA7 transfers;
  transfer support does not yet synthesize audio
- Native 960x544 monitor UI and Vita controls
- Host-side deterministic tests
- Redistributable PS2 ELF smoke-test generator

</details>

## Build the host tests

```sh
cmake -S . -B build-host -DCMAKE_BUILD_TYPE=Debug
cmake --build build-host
ctest --test-dir build-host --output-on-failure
```

## Build the Vita VPK

Install VitaSDK in WSL2 using the current [official VitaSDK instructions](https://vitasdk.org/),
then from the repository directory:

```sh
export VITASDK=/usr/local/vitasdk
export PATH="$VITASDK/bin:$PATH"
./scripts/build-vita.sh
```

This creates `build-vita/ps2vita.vpk`. The CMake packaging follows VitaSDK's
official [`vita_create_vpk`](https://github.com/vitasdk/vita-toolchain/blob/master/cmake_toolchain/vita.cmake)
workflow.

## First hardware test

1. Install `ps2vita.vpk` using VitaShell on a homebrew-enabled Vita.
2. Launch AstraRecomp. The built-in Phase-0 probe runs automatically. A correct
   build shows `PS2R AOT T0: PASS`, `PS2R AOT T1 CALL/RET: PASS`, and
   `AOT PERF: PASS`.
3. Vita3K validates correctness only. Copy
   `ux0:data/ps2vita/benchmark.txt`; do not use its speedup as a hardware result.
4. Press Cross to run the interpreter/bootstrap path. Its built-in test stops on
   `break` and shows `V0: 0000002A`.
5. To test the external loader too, generate the same program on the computer:

   ```sh
   python3 tools/make_smoke_elf.py smoke.elf
   ```

6. Create `ux0:data/ps2vita/` and copy `smoke.elf` there as `boot.elf`.
7. Press Triangle to load, then Cross to run.
8. The program also writes 42 to
   emulated address `0x2000` (the write is covered by the host tests).

Controls: Triangle reloads an ELF, Circle loads `ux0:data/ps2vita/bios.bin`, Cross
runs/pauses, Square single-steps, R toggles the GS diagnostic, Start resets, and
Select exits.

If a correctly sized `bios.bin` is present, PS2Vita discovers and maps it at
startup and begins executing it automatically. Emulator stops write
`ux0:data/ps2vita/fault.txt` with the
PC, opcode, key registers, and COP0 state for iterative BIOS bring-up.

## ELF and BIOS notes

The loader accepts bare-metal PS2 homebrew ELF files whose `PT_LOAD` segments fit
in EE RAM. Programs that call Sony's kernel will currently stop at `syscall` because
kernel HLE is not implemented. A PS2 BIOS is copyrighted and is deliberately not
included. A 4 MiB user-provided dump can now be mapped and started at the EE reset
vector, but it will stop when it reaches an unimplemented device or instruction.
Use a dump from hardware you own as permitted by your local law.

See [docs/PS2RECOMP_INTEGRATION.md](docs/PS2RECOMP_INTEGRATION.md) for the
Phase-0 frontend boundary, [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for core
design boundaries, [docs/BIOS_OPCODE_PROFILE.md](docs/BIOS_OPCODE_PROFILE.md) for
the measured EE/IOP optimization priorities,
[docs/EXECUTION_CENSUS.md](docs/EXECUTION_CENSUS.md) for deterministic dynamic
block/edge profile capture, and
[docs/ROADMAP.md](docs/ROADMAP.md) for the route from this milestone to games.
The benchmark methodology and physical-hardware decision thresholds are in
[docs/PERFORMANCE_VALIDATION.md](docs/PERFORMANCE_VALIDATION.md).

## License

Original AstraRecomp code is MIT; see [LICENSE](LICENSE). PS2Recomp is GPLv3 and
is currently kept as a separate host-side checkout/tool. Do not copy or distribute
upstream implementation code inside an MIT-only release without resolving the
combined-work licensing model.
