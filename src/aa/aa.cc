// aa.cc — RDP-adapted coverage AA resolve + runtime enable. Stream R.3-AA.
// Orthodox C++: POD + free functions, C headers, C-style casts, integer-only
// (no float in the per-pixel path -> host<->device bit-identical). See aa.h.
#include "aa/aa.h"

#include <stdint.h>

#include "gfx/framebuffer.h"  // rgb565()
#include "rdr/config.h"

// Runtime enable, default OFF. File-scope static reached only through the
// accessors below (CLAUDE.md: encapsulation, not a global hazard). The demo /
// golden path leaves this OFF so the renderer stays bit-identical to pre-AA;
// the Lead flips it ON at T4 integration (then rebakes the golden + runs
// on-target). The raster coverage WRITE and aa_resolve_tile both consult it.
static int s_aa_enabled = 0;

void aa_set_enabled(int on) { s_aa_enabled = (on != 0) ? 1 : 0; }

int aa_enabled(void) { return s_aa_enabled; }

// ---- resolve --------------------------------------------------------------
// SCHEME (deterministic, fixed scan order, fixed memory):
//   Footprint = a 3-TAP HORIZONTAL arm {left, centre, right}. Per RGB565
//   channel, nmin/nmax = min/max of the in-tile taps' channel values; an
//   out-of-tile tap (x == 0 left edge, x == RDR_TILE_W-1 right edge) is DROPPED
//   from the set (border-clamp -> never reads an adjacent tile / another
//   worker's FB -> the dual-core bit-identical invariant holds; the accepted
//   1px tile-seam, plan Q11).
//   RDP offset form per channel:
//     out = centre + ((nmin + nmax - 2*centre) * (FULL - c)) >> AA_SHIFT
//   where c = cov[i] in [0,255], FULL = 255. EDGE pixels only (cov < FULL);
//   interior (cov == FULL) pixels are SKIPPED untouched -> cost ~ edge length.
//
//   IN-PLACE HAZARD: a horizontal arm reads x-1 which, scanning left->right,
//   was already resolved this row -> a later read would see modified data and
//   diverge from the oracle. We avoid it WITHOUT a full-tile snapshot by
//   keeping ONE small ORIGINAL-row buffer (the row's pre-resolve colours):
//   AA_ROW bytes on the stack, asserted against a stated budget. Rows are
//   independent (the footprint is horizontal-only), so a single row buffer
//   suffices and the resolve is order-independent within a row.
enum { AA_FULL = 255, AA_SHIFT = 8 };

// Per-channel RDP offset blend toward the {nmin,nmax} midpoint, weighted by the
// pixel's *uncovered* fraction (FULL - c). centre/nmin/nmax are 8-bit channel
// values; c is the coverage byte. Integer-only, clamped to [0,255].
static uint8_t aa_blend_chan(int centre, int nmin, int nmax, int uncovered) {
  int const delta = (nmin + nmax) - (2 * centre);
  int out = centre + ((delta * uncovered) >> AA_SHIFT);
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return (uint8_t)out;
}

// Unpack RGB565 to 8-bit channels (bit-replicated; matches gfx/framebuffer.h
// rgb565() layout R[15:11] G[10:5] B[4:0]).
static void aa_unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

void aa_resolve_tile(uint16_t* fb, const uint8_t* cov, int tile_px_x0,
                     int tile_px_y0) {
  if (fb == 0 || cov == 0 || aa_enabled() == 0) {
    return;  // AA off / no coverage -> no-op (bit-identical to pre-AA).
  }
  // ORIGINAL-row buffer: the un-resolved RGB565 of the row currently being
  // resolved, so a horizontal tap reads pre-resolve colour regardless of write
  // order. 60 * 2 B = 120 B on stack. static_assert against a stated budget —
  // the RP2040 core1 stack is 2 KB (no project override) and the dual-core
  // raster runs ON the core stacks; a full-tile snapshot (60*60*2 = 7.2 KB)
  // would blow it (R.2 lesson). 120 B is comfortable.
  uint16_t row_orig[RDR_TILE_W];
  static_assert(sizeof(row_orig) <= 256,
                "AA resolve row scratch must stay <= 256 B (RP2040 core1 stack "
                "is 2 KB; a full-tile snapshot would overflow, invisible to "
                "host CI)");

  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    int const sy = tile_px_y0 + ty;
    // Snapshot this row's ORIGINAL colours before resolving any pixel in it.
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      row_orig[tx] = fb[(sy * RDR_SCREEN_W) + (tile_px_x0 + tx)];
    }
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      int const c = (int)cov[(ty * RDR_TILE_W) + tx];
      if (c >= AA_FULL) {
        continue;  // interior / untouched -> edge-gated skip.
      }
      uint8_t cr;
      uint8_t cg;
      uint8_t cb;
      aa_unpack565(row_orig[tx], &cr, &cg, &cb);
      int rmin = (int)cr;
      int rmax = (int)cr;
      int gmin = (int)cg;
      int gmax = (int)cg;
      int bmin = (int)cb;
      int bmax = (int)cb;
      // LEFT tap (in-tile only).
      if (tx > 0) {
        uint8_t lr;
        uint8_t lg;
        uint8_t lb;
        aa_unpack565(row_orig[tx - 1], &lr, &lg, &lb);
        if ((int)lr < rmin) {
          rmin = (int)lr;
        }
        if ((int)lr > rmax) {
          rmax = (int)lr;
        }
        if ((int)lg < gmin) {
          gmin = (int)lg;
        }
        if ((int)lg > gmax) {
          gmax = (int)lg;
        }
        if ((int)lb < bmin) {
          bmin = (int)lb;
        }
        if ((int)lb > bmax) {
          bmax = (int)lb;
        }
      }
      // RIGHT tap (in-tile only).
      if (tx < RDR_TILE_W - 1) {
        uint8_t rr;
        uint8_t rg;
        uint8_t rb;
        aa_unpack565(row_orig[tx + 1], &rr, &rg, &rb);
        if ((int)rr < rmin) {
          rmin = (int)rr;
        }
        if ((int)rr > rmax) {
          rmax = (int)rr;
        }
        if ((int)rg < gmin) {
          gmin = (int)rg;
        }
        if ((int)rg > gmax) {
          gmax = (int)rg;
        }
        if ((int)rb < bmin) {
          bmin = (int)rb;
        }
        if ((int)rb > bmax) {
          bmax = (int)rb;
        }
      }
      int const uncovered = AA_FULL - c;
      uint8_t const orr = aa_blend_chan((int)cr, rmin, rmax, uncovered);
      uint8_t const org = aa_blend_chan((int)cg, gmin, gmax, uncovered);
      uint8_t const orb = aa_blend_chan((int)cb, bmin, bmax, uncovered);
      fb[(sy * RDR_SCREEN_W) + (tile_px_x0 + tx)] = rgb565(orr, org, orb);
    }
  }
}
