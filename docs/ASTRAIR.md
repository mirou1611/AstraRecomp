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

## Scalar width inference

The first width pass classifies supported results as `I1`, `U16`, `S32`,
`U64`, or `V128` and records whether upper bits are zero, sign-extended from
bit 31, or unknown. It then propagates facts through each straight-line data
sequence. This preserves sign extension through `LUI -> ORI`, recognizes
zero-based `ORI` values as `U16`, and narrows `AND` when an operand proves the
result's upper bits are zero.

Unknown inputs remain `U64`; packed operations remain `V128`; writes to `$zero`
produce no result. The emitter consumes the refined instruction records but
does not optimize from these facts yet. This keeps Commit 6 independently
testable and leaves actual state-load/store elimination to the liveness pass.

## GPR liveness foundation

The liveness pass assigns exact low-GPR read/write sets to every supported IR
operation and computes `live_in`, `live_out`, and overwritten-dead writes by
walking a straight-line block backward. Register zero is excluded from use/def
sets, and callers provide the live-out contract explicitly.

This checkpoint does not eliminate or defer writes. That separation is
intentional: exits through memory faults, interpreter fallback, delay slots,
and indirect control flow must all materialize the same architectural state.
The subsequent emitter change will be feature-flagged and use these sets to
flush dirty locals on every exit before it can become the default.

## Deferred low-GPR writeback

Generated functions can cache only the nonzero low GPRs they reference and
materialize only dirty locals immediately before every existing `commit()`.
This covers normal direct/indirect/stop exits and memory-fault/interpreter
fallback exits without changing PC or cycle accounting. Packed high halves and
HI/LO remain architectural state operations. `ASTRART_DEFER_GPR_WRITEBACK=OFF`
restores the legacy emitter exactly; its generated-source SHA-256 remains the
established `F70A...BB5A` oracle.

The host Release probe uses nine internal median samples at 20,000 iterations.
Across three independent matched runs, the median AOT time changed from
162.130 ms with the legacy emitter to 160.304 ms with deferred writeback, about
1.1% faster. This is a narrow x86 result, not a physical-Vita performance
claim. The switch remains available until Vita measurements establish whether
the ARMv7 backend benefits.

## Profile-guided direct trace selection

The first trace-formation checkpoint accepts block frequencies, sampled edge
counts, and statically proven direct successors as separate inputs. Starting
from hottest unowned entries, it follows only a uniquely hottest successor
whose observed probability meets the configured threshold. It stops at hard
IR barriers, indirect/unproven targets, ambiguous ties, already-owned blocks,
the maximum length, and any block with more than one statically possible
successor. A hot edge back to the entry records a closed loop.

Keeping sampled edges separate from the static direct-successor relation is a
safety property: a census can observe an indirect destination, but observation
alone never turns it into a guard-free direct trace edge. Selection is fully
deterministic. This checkpoint does not yet change emitted functions; a profile
must first be matched to the exact ELF fingerprint and CFG.

Even a 99%-taken conditional branch is not guard-free. It is deliberately left
at the trace boundary until branch guards and matching deopt maps are consumed
by an executable trace emitter.

## Fingerprint-bound trace plans

The generator can now consume an execution census only when its `source`
metadata names the ordered ELF-set fingerprint used by the existing AOT package
contract:

```json
"source": {
  "kind": "elf-set",
  "fingerprint_scheme": "sha256-length-prefixed-elf-set-v1",
  "fingerprint_sha256": "<64 lowercase hexadecimal digits>"
}
```

The fingerprint hashes each input ELF's little-endian 64-bit byte length and
bytes, in command-line order. Missing source metadata, a different schema or
version, malformed addresses/counts, duplicate blocks/edges, and any fingerprint
mismatch are hard generator errors. In particular, a BIOS-only census cannot be
silently applied to an unrelated title ELF.

`--execution-census PROFILE.json --trace-plan-output PLAN.json` produces a
versioned, deterministically sorted JSON plan. Profile blocks and edges absent
from the generator's recovered CFG are discarded, and an observed edge is
followed only if static decoding independently proves it direct. Memory and
other hard-effect blocks terminate a trace. This stage writes review metadata
only and does not alter generated C++ or runtime dispatch.

## Guard and deoptimization metadata

Trace-plan schema version 1 now reserves explicit `guards` on every trace and a
root `deopt_maps` table. Current direct traces emit both as empty arrays. Future
branch-direction guards must reference an existing deopt map whose aligned
resume PC exactly equals the guard's side-exit PC.

Each deopt map can describe low/high guest-register recovery from architectural
state, a trace-local value, or a constant. Validation rejects missing/duplicate
map IDs, duplicate register-half recoveries, invalid registers or widths, and
inconsistent side exits before serialization. No speculative trace is emitted;
this establishes the reconstruction contract first.

## Executable guard-free wrappers

`ASTRART_DIRECT_TRACES=ON` now lets an exactly matched trace plan replace a
trace-entry table function with a generated wrapper. The wrapper calls each
already-generated block directly, verifies every intermediate exit is the
unique expected `Direct` target, accumulates guest-instruction counts, and
returns any unexpected exit immediately. Intermediate blocks retain their own
table entries so interpreter fallback or future deoptimization can resume there.

The default is `OFF`. The build creates a deterministic fingerprint-bound oracle
census and trace plan for the synthetic two-block call chain, allowing both modes
to compile and run through the full C++ differential suite. In trace mode, one
dispatch-budget unit advances from `0x3000` through `0x3020` to return site
`0x3008` while preserving the same five retired instructions and architectural
state.

This first executable form removes the dispatcher lookup between proven blocks,
but deliberately retains each block's normal commit path. It is a correctness
checkpoint, not yet cross-block register/state fusion and not yet a Vita speedup
claim. Guarded conditional traces remain rejected.

The runtime now exposes the conservative event-distance contract described in
`docs/EVENT_HORIZON.md`. Trace wrappers do not consume it yet; this ordering is
intentional so state fusion cannot silently cross a DMA, timer, video, or IOP
clock boundary.
