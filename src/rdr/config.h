// rdr/config.h — compile-time build configuration (Stream A).
// Capacity constants are the renderer-spec defaults; PENDING S0.5 on-device
// confirmation of free RAM / transform:raster ratio before final lock.
#ifndef RDR_CONFIG_H
#define RDR_CONFIG_H

// ---- color format (exactly one) --------------------------------------------
#define RDR_COLOR_565 1   // preferred for 3D (no Gouraud banding)
#define RDR_COLOR_4444 0  // fallback / fast bring-up (coverage in free nibble)

// ---- AA coverage storage ---------------------------------------------------
#define RDR_COVERAGE_PER_TILE \
  1  // 565 + 3000-tri build (per-tile scratch resolve)
#define RDR_COVERAGE_FULLSCR 0  // global resolve (4444 nibble / low poly)

// ---- display ---------------------------------------------------------------
#define RDR_SCREEN_W 240
#define RDR_SCREEN_H 240
#define RDR_TILE_W 60  // 16 tiles; Phase-15 tunable
#define RDR_TILE_H 60

// ---- capacities (S0.5-CONFIRMED: 138 KiB free beside FB > budget; see
//      docs/superpowers/specs/hardware-measurements.md). Sized to ~3000-tri. --
// T0: dropped 3000->2400. Post-G1 vertex sharing makes peak pool occupancy
// ~scene-vert-count, not tris*3. Measured peak over the full scripted loop at
// real density (1024 terrain + 252 tree tris) = 1827 tverts (PeakTvert probe).
// T5 SRAM reclaim: 2400->2048 (12% headroom over the 1827 scripted peak) frees
// ~6.9 KB toward the L6 XLU front-to-back accumulators. Overflow drops-with-
// count (graceful, never corrupts); PeakTvert guard keeps the cap above peak.
// NOTE free-fly (BTN_A) framing is unmeasured — re-check the probe if it ships.
#define RDR_MAX_TVERTS 2048  // transformed-vertex pool (20 B each)
#define RDR_MAX_TRIS 3000    // bin triangle cap (drop-with-count on overflow)
// T5 SRAM reclaim: the per-frame scratch arena is VESTIGIAL — arena_alloc() is
// called nowhere in src/ (only its own unit test); the Wave-E variable bins
// moved to the fixed Frame.bin_pool/bin_jobs arrays, leaving this 16 KB buffer
// dead (still init'd+reset each frame, never allocated from). Shrunk 16
// KiB->256 B (~15.7 KB reclaimed toward L6); kept nonzero so the
// arena_init/reset call sites stay valid and any future transient alloc has a
// home.
#define RDR_CMD_ARENA_BYTES 256

#endif  // RDR_CONFIG_H
