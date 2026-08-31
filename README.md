<p align="center">
  <img src="docs/assets/astra-hero.png" alt="Abstract streams of legacy machine code passing through a crystalline translation core into a compact runtime" width="100%">
</p>

<h1 align="center">AstraRecomp</h1>

<p align="center">
  <strong>Translate the past. Run it close to the metal.</strong><br>
  An experimental PS2 static-recompilation and hybrid runtime for homebrew-enabled PlayStation Vita systems.
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
  <a href="CONTRIBUTING.md">Contributing</a> ·
  <a href="docs/BRAND.md">Visual system</a>
</p>

AstraRecomp is an experimental PC-assisted PlayStation 2 static-recompilation and
hybrid porting framework for a homebrew-enabled PlayStation Vita. Heavy analysis
and translation happen on the PC; the Vita runs a small native runtime called
**AstraRT**. The existing Emotion Engine interpreter remains a correctness oracle,
BIOS/bootstrap path, and future cold-code fallback rather than the final fast path.

The current milestone is intentionally small but real: PS2Recomp analyzes the
initial MIPS corpora, while an independently written subset backend now reads the
ELF code and automatically emits compact AstraRT C++. Differential tests execute
both interpreter and native paths. The monitor shows `PS2R AOT T0: PASS` when
register, memory, PC, cycle, and stop state agree. A second probe shows
`PS2R AOT T1 CALL/RET: PASS` after chaining generated blocks through a real
JAL/delay-slot/`jr $ra` sequence.

It now executes a user-supplied retail BIOS through EE hardware initialization,
relocation into RAM, IOP reset-ROM startup, DECI2 startup, bidirectional SIF boot
traffic, and repeated `Restart Without Memory Clear` completion.
It does **not yet render the PS2 startup or run retail games**. Those require deeper IOP, DMA,
interrupt, GIF/GS, SPU2, and disc emulation described in the roadmap. The existing
core is real BIOS execution, not a renamed frontend or a fake compatibility screen.

## Project status

| Area | Current state |
| --- | --- |
| BIOS bootstrap | Executes through DECI2 startup, multi-tag SIF transfers, relocated EE code, and repeated no-clear restart completion |
| PC recompiler | Phase-0 analysis and a tested R5900-to-C++ subset with interpreter/native differential checks |
| Vita runtime | VitaSDK-only VPK, native monitor, ELF loading, stepping, and diagnostic framebuffer |
| Guest graphics | Reference rasterizer exists; guest GIF packets are not connected yet |
| Retail games | **Not playable**—IOP devices, GIF/GS, VU, SPU2, media, and compatibility work remain |

## System shape

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
  BEQ/BNE, delay slots, BREAK, and exact interpreter fallback
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
   build shows both `PS2R AOT T0: PASS` and `PS2R AOT T1 CALL/RET: PASS`.
3. Press Cross to run the interpreter/bootstrap path. Its built-in test stops on
   `break` and shows `V0: 0000002A`.
4. To test the external loader too, generate the same program on the computer:

   ```sh
   python3 tools/make_smoke_elf.py smoke.elf
   ```

5. Create `ux0:data/ps2vita/` and copy `smoke.elf` there as `boot.elf`.
6. Press Triangle to load, then Cross to run.
7. The program also writes 42 to
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
the measured EE/IOP optimization priorities, and
[docs/ROADMAP.md](docs/ROADMAP.md) for the route from this milestone to games.

## License

Original AstraRecomp code is MIT; see [LICENSE](LICENSE). PS2Recomp is GPLv3 and
is currently kept as a separate host-side checkout/tool. Do not copy or distribute
upstream implementation code inside an MIT-only release without resolving the
combined-work licensing model.
