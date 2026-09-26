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
