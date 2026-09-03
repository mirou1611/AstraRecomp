# Event-horizon contract

`Memory::cycles_until_next_event()` reports the conservative number of EE guest
cycles until the next possible device-model state transition. It is intended to
gate future AOT batching: a trace may defer scheduler advancement only when its
entire cycle cost is strictly smaller than the reported distance.

The initial horizon is the minimum of:

- the next IOP clock edge, including its fractional eight-to-one EE phase;
- the next HBlank edge;
- the next video-field/VBlank edge;
- active SIF0 or SIF1 completion countdowns;
- active SPU2 core DMA completion countdowns; and
- one cycle when a SIF channel is armed but `advance()` has not scheduled its
  countdown yet.

Treating every IOP clock edge as a boundary is intentionally conservative. A
later revision may calculate exact Timer 5 and IOP interrupt deadlines to expose
longer event-free spans. Returning a horizon that is too short costs performance;
returning one that is too long can change guest-visible ordering, so uncertainty
must always shorten the horizon.

The first pure direct-trace emitter now checks this contract before batching. A
five-cycle synthetic trace fuses only when the reported distance is greater than
five; a distance of exactly five falls back before executing its original first
block. MMIO/memory effects and effectful delay slots remain fusion barriers.
