# BIOS framebuffer capture

The host tracer can export the final 160x112 software framebuffer as a binary
RGB PPM image. This is diagnostic guest output, not the Vita monitor UI, and
does not reproduce a physical GS display/scanout pipeline.

After building the host tools, run from the repository root with your own BIOS:

```sh
./build-release/ps2bios_trace bios.bin 0 248800000 1 0 8 0 0 0 \
  build-release/framebuffer-census.json build-release/bios-framebuffer.ppm \
  > build-release/framebuffer-2488m.txt 2>&1
```

The last two positional arguments are the execution census JSON and framebuffer
PPM paths. Existing invocations without a framebuffer path are unchanged. Choose
fresh output paths: these diagnostic files are overwritten when supplied again.
Parent directories must already exist. Exit 1 is expected when this invocation
reaches its step budget rather than a requested stop PC; check `reason=running`
in the log. Exit 2 indicates an input/output or usage error.

The image stores RGB bytes in top-to-bottom row order, excluding alpha. Compare
`nonzero_rgb_pixels` with `nonzero_pixels`: the latter counts entire 32-bit pixels
and can include black pixels with nonzero alpha. The framebuffer hash still uses
the original full pixels, preserving comparisons with previous replays.

Open the PPM in an image viewer that supports Netpbm, or convert it losslessly to
PNG locally. Keep BIOS-derived snapshots and replay dumps local while debugging;
do not commit BIOS or game assets. Tests cover channel order, row order, alpha
omission, exact payload length, and stream failure.

For a separate probe of the GS framebuffer selected by the enabled privileged
display circuit, append `LINEAR_DISPLAY_PPM` after `FIRST_VIF_BIN`. Use `-`
to skip an unwanted optional census or VIF capture:

```sh
./build-release/ps2bios_trace bios.bin 0 300000000 1 0 8 0 0 0 \
  - build-release/draw-preview.ppm - build-release/display-linear.ppm \
  > build-release/display-probe.txt 2>&1
```

The second image samples Astra's current **linear** GS memory at the selected
`DISPFB` base (FBP in 8 KiB units) and width on the same 160x112 quarter grid.
The log prints raw
and decoded `PMODE`, `DISPFB1/2`, and `DISPLAY1/2`, plus a separate hash and
nonblack pixel count. This is not native GS swizzled memory or accurate CRT
scanout; it requires exactly one enabled display circuit and a PSMCT32/24
framebuffer with nonzero width. Keep both views separate when comparing to a
reference. The first path remains the renderer's current draw-target preview.

In the 300M-step BIOS checkpoint, circuit 2 selects FBP `0x50`, giving a
linear base of `0xA0000` at FBW 10 (640 pixels). At this endpoint, rows 0-47
of the linear display probe match rows 64-111 of the draw preview byte for
byte in RGB. The base offset equals 256 native rows, or 64 rows at this
quarter-resolution preview scale. This is a useful addressing cross-check,
not evidence that either image is a hardware-accurate scanout.

The host tracer also enables bounded triangle tracing (first 64 drawing kicks).
Each `gif_triangle` line includes PRIM, XYOFFSET, SCISSOR, TEST and ZBUF from the
selected context, three host vertices `(x,y,z,AABBGGRR)`, and their original
decoded GS XYZ values. XYZF's fog field is not included in these decoded values.
ADC-suppressed assembly updates do not create records. Zero-area drawing kicks
can appear in the trace even though the rasterizer correctly gives them no
coverage. Tracing is off by default in the runtime and never caps rendering.

It also records up to 256 emitted sprites as `gif_sprite` lines with raw XYZ/UV
endpoints and the selected PRIM, XYOFFSET, SCISSOR, TEX0, TEX1, CLAMP, FRAME,
TEST, ALPHA, TEXA and RGBAQ values. It includes raw linear source and
post-draw target hashes/nonblack RGB counts on bounded preview grids.
`sequence` is the zero-based sprite emission
count, so a reference sprite can be matched by state and geometry rather than
by assuming the two emulators use the same draw number. TEX1 is captured for
diagnosis; Astra does not yet implement its texture-filter selection.

To freeze the draw target immediately after a particular zero-based sprite
emission, append its sequence and a PPM path after the linear display path.
For example, BIOS sprite 50 follows triangles aligned with PCSX2 reference
draw 98, then uses the same feedback-sprite XYZ/UV endpoints and key GS state
as reference draw 99. Its texture contents still differ:

```sh
./build-release/ps2bios_trace bios.bin 0 300000000 1 0 8 0 0 0 \
  - - - - 50 build-release/sprite-50.ppm build-release/sprite-50-source.ppm \
  > build-release/sprite-50.txt 2>&1
```

`-` now skips *each* optional output path, including both PPM paths. The
sprite capture is read-only diagnostic state and does not change the draw.
By default, execution continues after capture. Set `ASTRA_STOP_ON_SPRITE=1`
to stop between EE steps once the requested sprite has been captured; this
requires a sprite PPM path and returns success with `sprite_capture_stop=1`.
The step budget and host deadline still apply if the sprite is never reached.
The optional source PPM samples the sprite's PSMCT32/24
texture base on a 160x64 quarter grid **before** the draw; it uses Astra's
linear local-memory approximation, not native GS swizzling. A nonexistent
sequence is an explicit output error.

With sprite capture enabled, the trace also freezes the latest 64
nondegenerate triangles before that sprite as
`gif_sprite_preceding_triangle` records, retaining absolute triangle sequence
and draw state. This is a ring buffer, not the first-64 prefix. It let us
align Astra's source-producing triangles before sprite 50 with PCSX2
reference draw 98 by FRAME/TEX0/ALPHA and near-identical vertices.

An optional final path dumps the latest preceding PSMCT16 triangle texture
as headerless little-endian `uint16` texels in row-major order. The trace
prints its TEX0, dimensions, hash, and nonzero-RGB count. This reflects
Astra's linear local-memory model at the selected sprite, not a native GS
swizzled texture. For the matched draw-98 pass, use sprite sequence 50 and
append `build-release/sprite-50-texture16.bin` after the sprite source PPM.
The captured BIOS texture matched PCSX2 draw 98's raw 128x128 texture at
all 16,384 texels (the reference PNG displays each little-endian texel as
its low and high byte in R/G). This rules out the selected texture upload as
the cause of the much wider target mismatch; it does not validate native
PSMCT16 swizzling elsewhere.

## Isolated VIF replay

Append `FIRST_VIF_BIN` after the framebuffer path to save the first submitted
VIF1 stream, up to 1 MiB. Capture owns its bytes before VIF/VU execution and
survives later memory writes. It is disabled by default. Missing, empty or
oversized captures are reported as errors rather than written as partial data.

```sh
./build-release/ps2bios_trace bios.bin 0 248800000 1 0 8 0 0 0 \
  build-release/vif-capture-census.json build-release/vif-capture.ppm \
  build-release/first-vif.bin > build-release/vif-capture-2488m.txt 2>&1
./build-release/ps2vif_replay build-release/first-vif.bin build-release/vif-only.ppm
```

The replay tool starts with reset memory, VIF, VU and GS state, processes the
captured stream, then submits its path-1 packets to GIF. Its summary exposes
VU pair counts and rejection origin. Exit 1 indicates a processing rejection;
exit 2 indicates usage or I/O errors. Output files are overwritten if they exist.
The input size is checked before allocating its buffer.

An optional final `VU1_DATA_BIN` path exports all 16,384 bytes of final VU1
data memory, starting at guest `0x1100C000`, as raw little-endian bytes:

```sh
./build-release/ps2vif_replay build-release/first-vif.bin \
  build-release/vif-only.ppm build-release/vu1-final.bin
```

The dump is written even when the replay reports the known PATH1 rejection.
It supports word-by-word comparison with a reference run, but preserves only
final memory: earlier XGKICK payloads may already have been overwritten.
Append `--explain-vu-qword 01D0` after the dump path to walk the last recorded
writers and register ancestry of that aligned VU byte address. The argument is
exactly four hexadecimal digits in `0000..3FF0`. The breadth-first walk is
bounded to 64 nodes; missing ancestry stays explicitly incomplete.

For an independent PCSX2 final-memory oracle, use an existing profile containing
your selected BIOS. The runner makes a unique local profile, enables EE serial
logging, disables VU speed hacks, and stops only its own child process:

```sh
python tools/run_vu1_reference.py capture build-release/first-vif.bin \
  build-release/vu1-reference.bin \
  --pcsx2 .tools/pcsx2-v2.8.2/pcsx2-qt.exe \
  --profile-template .tools/pcsx2-reference-data/PCSX2
python tools/run_vu1_reference.py compare \
  build-release/vu1-reference.bin build-release/vu1-final.bin
```

This launch example uses the Windows PCSX2 executable. Comparison and fixture
generation also work on the Linux host. Outputs must be fresh filenames; compare
returns 1 on a difference and rejects non-16-KiB dumps. Runtime is bounded to
15 seconds by default (maximum 60). Metadata records the selected input, hashes,
settings and execution mode. `--vu1-interpreter` cross-checks PCSX2's interpreter
against its default microVU mode. A register-initialization microprogram and
explicit RAM clear precede the unchanged selected input; the result is still a
final-memory check, not a full CPU/VU savestate comparison or historical GIF oracle.

The replay also prints bounded `vu_div` records with the actual numerator,
denominator, pending result and ready cycle. These are observations, not values
reconstructed from final VF registers. An owned finite-arithmetic packet can
separate arithmetic behavior from the BIOS stream's upload/execution overlap:

```sh
python tools/make_vu1_math_fixture.py build-release/vu1-math.bin \
  --layout build-release/vu1-math.json
./build-release/ps2vif_replay build-release/vu1-math.bin \
  build-release/vu1-math.ppm build-release/vu1-math-astra.bin
```

Run the reference command above on this new packet in both modes. Each case
waits with FLUSHE before changing its inputs; outputs use distinct qwords
`000..00F`. The JSON contains raw operands and output locations, not native-float
goldens. Four MUL rows cite recorded VU0 MULi hardware data transposed to VU1 MUL;
this does not imply hardware validation of the fixture or every VU multiply.

`--suite div-boundaries` generates a second owned packet covering finite DIV
normalization, signed underflow, saturation and exponent boundaries. The
`vu1_math_vif_replay` CTest gate replays both suites and checks their complete
16-KiB memory hashes against observations from both PCSX2 v2.8.2 execution modes;
PCSX2 itself is not required for CI.

The unchanged BIOS-derived stream does not preserve DMA arrival timestamps.
Later uploads can overlap a running microprogram in the reference while Astra's
current functional VIF implementation executes calls sequentially. To isolate
arithmetic from that overlap, create an explicitly controlled reference:

```sh
python tools/run_vu1_reference.py capture build-release/first-vif.bin \
  build-release/vu1-functional-reference.bin \
  --pcsx2 .tools/pcsx2-v2.8.2/pcsx2-qt.exe \
  --profile-template .tools/pcsx2-reference-data/PCSX2 \
  --serialize-vu-starts
```

This flag inserts FLUSHE/three-NOP qwords after command-boundary MSCAL, MSCALF
and MSCNT. Original bytes and their modulo-16 alignment are preserved. Metadata
records original/selected/transformed hashes and every inserted barrier. Unknown
formats, noncontiguous STCYCL and truncated payloads are rejected; opcode-looking
payload words are never scanned as commands. The standalone
`tools/serialize_vif_fixture.py` can write the same transformed packet to a fresh
filename. This changes event ordering: it is a **sequential functional fixture,
not the original BIOS timeline, GIF history or hardware oracle**. Do not insert
such barriers into production emulation to make a picture agree.

`--prefix-bytes 0x8A4` ends the known local capture just after its first MSCAL;
`0x9CC` ends after its first MSCNT. These offsets are specific to this capture,
not universal BIOS constants. Take a complete command boundary from `vif_run`
records; the tool only checks word alignment and pads the prefix with NOPs to a
DMA qword. It cannot detect truncation inside an upload payload.

This is not a savestate: prior path-3 GS setup, textures, earlier VU state and
other device state are absent. Establish agreement for the specific failure
before relying on isolated replay. Its image is not the full BIOS image.
Keep the BIOS-derived binary capture and images local and uncommitted.

The BIOS tracer prints `vif_dma_span` records for the last successfully assembled
VIF DMA chain. Each maps a stream interval to its EE source address, including
separate intervals for TTE tag bytes. For stream offset `n` within a span,
the source address is `source + (n - offset)`. The mapping is built alongside
the actual byte copy; it is not reconstructed from final DMA registers. It is
cleared on memory reset and replaced by each successful chain. With multiple
submissions, do not mistake this last-chain mapping for the first captured stream.
