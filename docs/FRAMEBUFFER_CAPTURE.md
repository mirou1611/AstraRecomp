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
