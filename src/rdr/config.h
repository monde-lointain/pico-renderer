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
#define RDR_MAX_TVERTS 3000  // transformed-vertex pool (~14 B each)
#define RDR_MAX_TRIS 3000    // bin triangle cap (drop-with-count on overflow)
#define RDR_CMD_ARENA_BYTES (16 * 1024)

#endif  // RDR_CONFIG_H
