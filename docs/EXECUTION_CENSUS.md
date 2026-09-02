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

## Schema version 1

```json
{
  "schema": "astrarecomp.execution-census",
  "version": 1,
  "ee_steps": 200000000,
  "iop_divisor": 8,
  "ee": {
    "instructions": 199999907,
    "blocks": [
      {"pc": "0x00081FC0", "entries": 5538542}
    ],
    "edges": [
      {
        "source": "0x00081FC0",
        "target": "0x00081FC0",
        "transitions": 5538541
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

Addresses are fixed-width hexadecimal strings so consumers do not lose their
unsigned 32-bit representation. Blocks sort by PC and edges by source then
target, making repeated captures directly diffable. Counts use JSON integers.

## Current retail-BIOS reference capture

The 02.00E 200-million-step checkpoint produces:

| Processor | Retired instructions | Dynamic block PCs | Dynamic edges |
|---|---:|---:|---:|
| EE | 199,999,907 | 2,460 | 3,132 |
| IOP | 24,999,461 | 5,006 | 6,896 |

The JSON is 1,063,094 bytes. Census-enabled execution preserves the established
434 SIF0 activity transitions and the same final sampled CPU state as the
non-census replay.

This first schema deliberately contains only block and edge frequencies. Later
schema revisions should add indirect-target classification, register-width and
`$sp`/`$gp` observations, event spacing, MMIO poll candidates, and DMA sizes
without changing the meaning of version-1 fields.
