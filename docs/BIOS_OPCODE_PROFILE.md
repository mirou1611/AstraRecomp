# Retail BIOS dynamic opcode profile

This profile turns generator and interpreter priorities into a measured decision.
It was captured from the validated 2004-06-14 retail BIOS path after 30,000,000
EE scheduler steps, with the IOP interleaved at the hardware-derived 1:8 ratio.
The firmware itself is not distributed by this project.

Run it again with:

```sh
./build-host/ps2bios_trace BIOS 0 30000000 1 0 8
```

Families are normalized at the same decode boundaries as the interpreters:
register numbers and immediates do not split a family, while SPECIAL, REGIMM,
COP, and MMI selectors do. Hardware interrupt-entry cycles are excluded. The
trace prints the top 24 families for each processor automatically.

## EE top families

| Rank | Family | Instructions | Share |
| ---: | --- | ---: | ---: |
| 1 | SLL/NOP | 6,802,973 | 22.68% |
| 2 | LW | 3,049,294 | 10.16% |
| 3 | ADDIU | 3,014,714 | 10.05% |
| 4 | BNE | 3,006,152 | 10.02% |
| 5 | SLTU | 2,181,128 | 7.27% |
| 6 | SQ | 2,101,537 | 7.01% |
| 7 | PCPYUD | 2,093,357 | 6.98% |
| 8 | LUI | 1,822,048 | 6.07% |
| 9 | BEQ | 1,820,913 | 6.07% |
| 10 | SD | 809,476 | 2.70% |
| 11 | LD | 809,207 | 2.70% |
| 12 | AND | 608,148 | 2.03% |
| 13 | JR | 407,291 | 1.36% |
| 14 | JAL | 406,435 | 1.35% |
| 15 | ORI | 405,940 | 1.35% |
| 16 | ANDI | 404,620 | 1.35% |

The top eight families cover 80.24% of decoded EE instructions. The top sixteen
cover 99.14%. The subset generator now handles the scalar members LW, ADDIU,
BNE, SLTU, LUI, BEQ, AND, JR, JAL, ORI, ANDI, and NOP. The measured remaining
expansion order is therefore SQ, PCPYUD, SD, and LD. Those operations need wider
memory and 128-bit aliasing coverage rather than more scalar cases. PCPYUD's
unusually high count comes from the BIOS interrupt context path, so it is a real
boot-path priority rather than an assumed multimedia workload.

## IOP top families

| Rank | Family | Instructions | Share |
| ---: | --- | ---: | ---: |
| 1 | SLL/NOP | 973,605 | 25.96% |
| 2 | J | 667,303 | 17.79% |
| 3 | ADDIU | 439,519 | 11.72% |
| 4 | LW | 277,052 | 7.39% |
| 5 | SW | 208,058 | 5.55% |
| 6 | BNE | 154,938 | 4.13% |
| 7 | ADDU | 141,995 | 3.79% |
| 8 | LUI | 137,012 | 3.65% |
| 9 | ORI | 131,709 | 3.51% |
| 10 | BEQ | 112,096 | 2.99% |
| 11 | JR | 109,051 | 2.91% |
| 12 | JAL | 101,896 | 2.72% |
| 13 | BGTZ | 96,032 | 2.56% |
| 14 | SLTU | 49,708 | 1.33% |
| 15 | AND | 47,803 | 1.27% |
| 16 | SB | 29,416 | 0.78% |

The top eight IOP families cover 80.00%; the top sixteen cover 98.06%. This
supports keeping the R3000A fast path compact: basic control flow, scalar loads
and stores, and a small integer subset dominate the current BIOS workload.

## Decision

Correctness remains the gate, but AOT expansion should now follow measured
coverage rather than ISA-table order. Re-run this checkpoint whenever BIOS
progress crosses a new hardware boundary, and keep profiles scoped by milestone
so a later idle loop does not erase what was hot during initialization.
