# Minimal AstraIR

AstraIR is the semantic boundary between guest decoding and AstraRT C++
emission. Its first checkpoint is deliberately representation-only: it covers
exactly the data-instruction subset already accepted by
`tools/generate_astrart.py` and introduces no optimization.

The initial pipeline is:

```text
PS2 ELF words
  -> existing control-flow discovery
  -> tools/astrair/builder.py
  -> typed tools/astrair/ir.py instruction records
  -> existing deterministic C++ emission
  -> interpreter fallback for unsupported operations
```

The typed operation set currently includes scalar add/logic/compare/multiply,
the existing packed bitwise subset, immediate logic, and 32/64-bit loads and
stores. Each record preserves the guest PC, raw instruction word, register
fields, and signed immediate. Branches, jumps, delay-slot ownership, faults,
and package metadata remain in the established generator path.

This separation is intentionally smaller than the eventual IR. The following
belong in subsequent reviewable checkpoints:

- architectural effect flags and hard barriers;
- semantic width and upper-bit knowledge;
- liveness and deferred writeback;
- profile-guided trace formation;
- guards, deoptimization maps, and event-horizon use.

## Correctness contract

Unsupported encodings do not receive guessed semantics. The builder returns no
IR operation, and the existing generator emits its interpreter exit. Builder
contract tests enumerate every supported operation family and reject control
or unsupported data instructions.

For the eight deterministic Phase-0 ELF corpora, generation before and after
the IR seam produces byte-identical `phase0_aot_package.cpp` with SHA-256
`F70A5D3C7DC76263945A6976E90927ED187274A8BC36165A5FA6C3EC893FBB5A`.
The normal interpreter/AOT differential suite remains the execution oracle.

## Conservative effects and barriers

Each instruction now carries an `Effect` mask. Scalar and packed arithmetic are
`PURE`. Loads are marked `MEMORY_READ | MAY_FAULT | MAY_TOUCH_MMIO`; stores are
marked `MEMORY_WRITE | MAY_FAULT | MAY_TOUCH_MMIO | MAY_SCHEDULE_EVENT`.

These classifications are intentionally conservative because address
provenance does not exist yet. A future trace builder must stop at any
instruction that may fault, touch MMIO, or schedule a device event. Later
provenance analysis may prove a specific access to be ordinary RAM and replace
that barrier with guarded fast/fallback paths. The effect checkpoint itself
does not alter emitted code or runtime behavior.
