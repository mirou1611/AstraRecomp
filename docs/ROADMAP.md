# Roadmap to PS2 software

![Abstract synchronized processor and packet-flow visualization](assets/astra-architecture.png)

> AstraRecomp is a staged engineering effort. Milestones describe evidence and
> boundaries, not compatibility promises or release dates.

## M0 — ELF interpreter monitor (implemented)

Load a PS2 ELF, execute the integer subset, honor delay slots, inspect state, and
prove behavior with host tests plus a Vita VPK target.

## M1 — EE correctness

- MMI instructions using the implemented 128-bit GPR storage
- Unaligned loads/stores, traps, exceptions, TLB, COP0 state
- COP1 floating point and deterministic NaN behavior
- Timer and interrupt delivery
- Import a legal CPU conformance suite and fuzz decoder boundaries
- Add decoded basic-block metadata and page-granular invalidation while retaining
  the interpreter as the semantic oracle

## M2 — IOP and system scheduler

- R3000A interpreter and 2 MiB IOP RAM
- SIF communication, event scheduler, INTC, DMAC, counters
- Expand the implemented BIOS mapping/reset vector into full boot hardware support
- Trace comparison against PCSX2 for user-supplied test programs

## M3 — First pixels at 0.25x

- GIF packet parsing and GS register state
- Connect the implemented 160x112 CPU reference rasterizer to GIF packets and
  add nearest-neighbor upscale to 960x544
- Framebuffer, depth, scissor, alpha-test, texture, and blending basics
- Optional GXM backend only after the reference rasterizer is correct

## M4 — Vector units and audio

- VU0/VU1 interpreters and microprogram memory
- VIF unpacking and PATH arbitration
- SPU2 voices, ADPCM, mixing, and Vita audio output

## M5 — Media and compatibility

- CD/DVD image reader and IOP storage modules
- Controller mapping, memory cards, saves, configuration
- Per-title logs and compatibility database

## M6 — Performance

- Profile on real Vita hardware
- Probe Play!-CodeGen's AArch32 EABI backend under VitaSDK, then add an optional
  ARMv7 EE/IOP block JIT with exact-PC interpreter fallback
- Threaded VU1/GS where synchronization permits
- Dirty-page tracking, texture cache, frame skipping, and adaptive budgets

The Vita's CPU and memory bandwidth are far below a desktop running PCSX2. Even at
very low resolution, CPU/VU emulation—not pixel count alone—will dominate many
games. The practical first targets should be PS2 homebrew and simple 2D titles.
