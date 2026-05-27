# S0.5 Hardware Measurements (on-device probe)

Source: `tools/spike/spike_main.cc` (branch `spike/hw-probe`, throwaway), flashed via picotool,
read over USB-CDC. Date: 2026-05-27. Board: Pimoroni PicoSystem (RP2040).

## Raw KEY=VALUE
```
RAM_FB_BYTES=115200
RAM_FREE=138492
MULDIV_NS=576
FILL_MBPS=249
MICRORAST_TRIS_PER_S=25000000000   # UNRELIABLE (see caveats)
```

## Caveats (read before trusting absolutes)
- **Stock clock.** The spike used plain `pico_stdlib` and did NOT apply the PicoSystem 250 MHz /
  1.20 V overvolt the renderer targets. `MULDIV_NS` and `FILL_MBPS` are therefore CONSERVATIVE
  (~2x headroom expected at 250 MHz). Re-measure under the overclock (or rely on C3) for accurate
  throughput.
- **MICRORAST is bogus.** The micro-rasterizer inner loop was optimized away / timed sub-µs, so the
  2.5e10 figure is an artifact. The real transform:raster ratio is deferred to the **C3 throughput
  probe** on the integrated pipeline.

## Architecture decision (sort-middle vs immediate-forward + 8-bit Z)
- **Free RAM = 138 KiB beside the framebuffer**, exceeding the spec's ~120 KiB estimate. Sort-middle's
  retained transformed geometry (~66 KiB at 3000 tris) + per-tile Z/coverage scratch fit with margin.
- **DECISION: sort-middle tiling CONFIRMED** on the memory axis. The transform:raster-ratio half of
  the justification remains to be confirmed at C3; the immediate-forward + 8-bit-Z fallback stays
  documented in the renderer spec if C3 shows transform is unexpectedly cheap.
- **`config.h` capacities stand:** `RDR_MAX_TVERTS=3000`, `RDR_MAX_TRIS=3000` (well within 138 KiB).
