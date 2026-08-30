# AstraRecomp

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
relocation into RAM, IOP reset-ROM startup, and the BIOS `Initialize Done` stage.
It does **not yet
render the PS2 startup or run retail games**. Those require deeper IOP, DMA,
interrupt, GIF/GS, SPU2, and disc emulation described in the roadmap. The existing
core is real BIOS execution, not a renamed frontend or a fake compatibility screen.

## What works now

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
- Absent development-board debug aperture with retail-style null-device behavior
- Portable EE interpreter foundation with 128-bit GPR storage
- Fixed-size decoded EE block cache with delay-slot boundaries, hotness tracking,
  and guest-RAM page generation invalidation
- Integer arithmetic, branches and delay slots, jumps, multiply/divide, COP0
  register moves, unaligned merge operations, and byte through quadword memory ops
- Initial MMI support (`PADDUW`, `DIV1/DIVU1`, `HI1/LO1` moves) using full
  128-bit GPR state and the EE's second accumulator pair
- Scalar COP1 register moves, arithmetic, conversion, comparison, and branches
- Scalar COP1 memory transfers (`LWC1`/`SWC1`)
- Verified native semantic zero-fill acceleration for the BIOS's large RAM clear
- 160x112 software GS reference framebuffer with depth-tested points, lines, and triangles
- Native 960x544 monitor UI and Vita controls
- Host-side deterministic tests
- Redistributable PS2 ELF smoke-test generator

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
design boundaries, and [docs/ROADMAP.md](docs/ROADMAP.md) for the route from this
milestone to games.

## License

Original AstraRecomp code is MIT; see [LICENSE](LICENSE). PS2Recomp is GPLv3 and
is currently kept as a separate host-side checkout/tool. Do not copy or distribute
upstream implementation code inside an MIT-only release without resolving the
combined-work licensing model.
