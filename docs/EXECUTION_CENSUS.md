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

## Schema version 2

```json
{
  "schema": "astrarecomp.execution-census",
  "version": 2,
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
    ]
  },
  "iop": {
    "instructions": 24999461,
    "blocks": [],
    "edges": []
  }
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

## Current retail-BIOS reference capture

The 02.00E 200-million-step checkpoint produces:

| Processor | Retired instructions | Block PCs | Edges | Indirect sites | Monomorphic sites |
|---|---:|---:|---:|---:|---:|
| EE | 199,999,907 | 2,460 | 3,132 | 355 | 242 |
| IOP | 24,999,461 | 5,006 | 6,896 | 765 | 528 |

The version-2 JSON is 1,977,350 bytes. Of the observed block PCs, 2,044 EE and
3,633 IOP entries have a single `$gp` value across the capture. Census-enabled
execution preserves the established
434 SIF0 activity transitions and the same final sampled CPU state as the
non-census replay.

Schema version 1 contains block and edge frequencies. Version 2 adds block-entry
`$sp`/`$gp` ranges and validated indirect targets without changing version-1
field meanings. Later revisions should add register-width observations, event
spacing, MMIO poll candidates, and DMA sizes.
