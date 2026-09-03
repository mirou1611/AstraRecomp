# Execution census

`ps2bios_trace` can optionally emit a deterministic, machine-readable profile
of retired EE and IOP control flow. This is the first input to the planned
profile-guided AstraIR pipeline; it does not change CPU or device behavior.

## Capture

The final positional argument selects the JSON output path. The preceding
diagnostic arguments are zero when they are unused:

```sh
./build-trace-release/ps2bios_trace BIOS 0 200000000 1 0 8 0 0 0 \
  execution-census.json
```

The trace tool returns its existing nonzero max-step status after a bounded run,
but writes the census before returning. A stop-PC run returns success as usual.

Only instructions that actually retire are recorded. Interrupt entry itself is
not counted as execution of the interrupted instruction. MIPS delay slots remain
part of their source block, including branch-likely handling when the delay slot
is annulled. Interrupts, exceptions, indirect transfers, and other observed PC
discontinuities create dynamic block edges.

## Schema version 3

```json
{
  "schema": "astrarecomp.execution-census",
  "version": 3,
  "ee_steps": 200000000,
  "iop_divisor": 8,
  "ee": {
    "instructions": 199999907,
    "blocks": [
      {
        "pc": "0x00200E50",
        "entries": 2645315,
        "sp_min": "0x01FFE8E0",
        "sp_max": "0x01FFE8E0",
        "gp_min": "0x002CFFF0",
        "gp_max": "0x002CFFF0"
      }
    ],
    "edges": [
      {
        "source": "0x00081FC0",
        "target": "0x00081FC0",
        "transitions": 5538541
      }
    ],
    "indirect_targets": [
      {
        "site": "0x800001A0",
        "target": "0x80000280",
        "transitions": 289497
      }
    ],
    "mmio_reads": [
      {
        "site": "0x8000DCF0",
        "address": "0x1000F000",
        "width": 4,
        "reads": 1104408
      }
    ]
  },
  "iop": {
    "instructions": 24999461,
    "blocks": [],
    "edges": [],
    "indirect_targets": [],
    "mmio_reads": []
  },
  "events": [
    {
      "kind": "sif0_start",
      "count": 217,
      "min_gap": 5584,
      "max_gap": 55337856,
      "average_gap": 738504
    }
  ]
}
```

Addresses and register bounds are fixed-width hexadecimal strings so consumers
do not lose their unsigned 32-bit representation. Blocks sort by PC; edges sort
by source then target; indirect targets sort by site then target. This makes
repeated captures directly diffable. Counts use JSON integers.

The `$sp`/`$gp` bounds are sampled at dynamic block entry. Indirect-target rows
cover `JR` and `JALR`. A target is counted only when the next retired block PC
matches the source register captured at the indirect instruction, preventing an
interrupt after the delay slot from being mislabeled as an indirect target.
MMIO rows aggregate retired load instructions by site, normalized physical
address, and access width. Event rows summarize gaps in EE master steps between
observed event edges of the same kind. They are observational statistics, not
yet a scheduler `next_event_cycle()` contract.

## Current retail-BIOS reference capture

The 02.00E 200-million-step checkpoint produces:

| Processor | Retired instructions | Block PCs | Edges | Indirect sites | Monomorphic sites |
|---|---:|---:|---:|---:|---:|
| EE | 199,999,907 | 2,460 | 3,132 | 355 | 242 |
| IOP | 24,999,461 | 5,006 | 6,896 | 765 | 528 |

The version-3 JSON is 2,019,292 bytes. Of the observed block PCs, 2,044 EE and
3,633 IOP entries have a single `$gp` value across the capture. Census-enabled
execution preserves the established 434 SIF0 activity transitions and the same
final sampled CPU state as the non-census replay.

The capture contains 172 EE site/address MMIO pairs totaling 2,297,997 reads and
345 IOP pairs totaling 355,171 reads. The largest late IOP site is
`0x00097500 -> 0x1F900744` with 222,884 16-bit reads, directly identifying the
then-current SPU2 polling loop. The event census observes 217 SIF0 starts and 217
completions, 79 EE interrupt rising edges, 540 IOP interrupt rising edges, 86
SIF1 DMA starts, one SPU2 DMA4 start, and two SPU2 DMA7 starts. No VIF0, VIF1,
or GIF DMA start is observed after initialization.

## Post-STATX correctness capture

Decoding the census hotspot showed that `0x1F900744` is SPU2 core 1 `STATX`.
The BIOS routine at `0x000974D0` waits for ready bit `0x0080`, but the old
register-file-only model left the status at zero and forced the guest through a
`1 << 24` software timeout. SPU2 now resets ready, changes to busy `0x0400` at
DMA start, and restores ready when a DMA-mode transfer completes.

At the same 200-million-step boundary, the corrected capture changes the
observations as follows:

| Observation | Before | After |
|---|---:|---:|
| SIF0 activity transitions | 434 | 452 |
| IOP MMIO reads | 355,171 | 140,236 |
| IOP block PCs | 5,006 | 5,016 |
| IOP edges | 6,896 | 6,922 |
| SIF0 starts/completions | 217 / 217 | 226 / 226 |
| SIF1 DMA starts | 86 | 95 |
| SPU2 DMA7 starts | 2 | 5 |

The 222,884-read `0x00097500 -> 0x1F900744` timeout disappears, and the sampled
IOP endpoint moves from `0x000974F8` to `0x00093F10`. The EE remains running at
`0x00081FC0`. No VIF0, VIF1, or GIF DMA start is observed yet, so this is boot
and device-initialization progress rather than graphics progress.

Schema version 1 contains block and edge frequencies. Version 2 adds block-entry
`$sp`/`$gp` ranges and validated indirect targets without changing version-1
field meanings. Version 3 adds MMIO read aggregation and observed event spacing.
Later revisions should add register-width observations and DMA-size histograms.

## Compiler identity boundary

Raw BIOS census output remains diagnostic and is not accepted as title-ELF
optimization input. Before a version-3 census may guide AstraIR trace planning,
the capture pipeline must bind it to the exact ordered ELF set with this root
metadata:

```json
"source": {
  "kind": "elf-set",
  "fingerprint_scheme": "sha256-length-prefixed-elf-set-v1",
  "fingerprint_sha256": "<AOT source fingerprint>"
}
```

`tools/generate_astrart.py` recomputes the fingerprint from its ELF inputs and
fails closed on missing or mismatched metadata. It also intersects the accepted
profile with its independently recovered static CFG before writing a trace plan.
The plan is inspectable metadata; profile data does not yet change emitted code.
