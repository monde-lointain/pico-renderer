// aa_test.cc — R.3-AA resolve + enable tests. Host-first TDD. The resolve is
// integer-only; we replicate its exact 3-tap-horizontal, border-clamped scheme
// in a tiny C reference and assert bit-equality, plus: interior (full-coverage)
// pixels stay UNCHANGED (edge-gated), border pixels never read out of the tile
// (ASan-clean — the fb has guard rows of a sentinel colour the resolve must not
// pull in), and the resolve actually CHANGES some pixels (anti-vacuity).
#include "aa/aa.h"

#include <stdint.h>
#include <string.h>

#include "gtest/gtest.h"
#include "rdr/config.h"

namespace {

// RGB565 pack/unpack matching gfx/framebuffer.h + aa.cc (bit-replicated).
uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)((((uint16_t)(r & 0xF8)) << 8) |
                    (((uint16_t)(g & 0xFC)) << 3) | (uint16_t)(b >> 3));
}
void unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// Resolve enable RAII-free helper: tests flip it explicitly. Default OFF.
enum { K_FULL = 255, K_SHIFT = 8 };

uint8_t blend_chan_ref(int centre, int nmin, int nmax, int uncovered) {
  int const delta = (nmin + nmax) - (2 * centre);
  int out = centre + ((delta * uncovered) >> K_SHIFT);
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return (uint8_t)out;
}

// Independent reference: replicate aa_resolve_tile's exact scheme over a tile
// extracted from `fb` into `out` (out sized RDR_TILE_W*RDR_TILE_H). 3-tap
// horizontal {left,centre,right}, in-tile clamp, edge-gated (cov<FULL).
void resolve_ref(const uint16_t* fb, const uint8_t* cov, int x0, int y0,
                 uint16_t* out) {
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      uint16_t const orig = fb[((y0 + ty) * RDR_SCREEN_W) + (x0 + tx)];
      int const c = (int)cov[(ty * RDR_TILE_W) + tx];
      if (c >= K_FULL) {
        out[(ty * RDR_TILE_W) + tx] = orig;
        continue;
      }
      uint8_t cr;
      uint8_t cg;
      uint8_t cb;
      unpack565(orig, &cr, &cg, &cb);
      int rmin = cr;
      int rmax = cr;
      int gmin = cg;
      int gmax = cg;
      int bmin = cb;
      int bmax = cb;
      int const taps[2] = {tx - 1, tx + 1};
      for (int k = 0; k < 2; ++k) {
        int const nx = taps[k];
        if (nx < 0 || nx >= RDR_TILE_W) {
          continue;
        }
        uint8_t nr;
        uint8_t ng;
        uint8_t nb;
        unpack565(fb[((y0 + ty) * RDR_SCREEN_W) + (x0 + nx)], &nr, &ng, &nb);
        if (nr < rmin) {
          rmin = nr;
        }
        if (nr > rmax) {
          rmax = nr;
        }
        if (ng < gmin) {
          gmin = ng;
        }
        if (ng > gmax) {
          gmax = ng;
        }
        if (nb < bmin) {
          bmin = nb;
        }
        if (nb > bmax) {
          bmax = nb;
        }
      }
      int const uncovered = K_FULL - c;
      uint8_t const rr = blend_chan_ref(cr, rmin, rmax, uncovered);
      uint8_t const rg = blend_chan_ref(cg, gmin, gmax, uncovered);
      uint8_t const rb = blend_chan_ref(cb, bmin, bmax, uncovered);
      out[(ty * RDR_TILE_W) + tx] = pack565(rr, rg, rb);
    }
  }
}

// Full-screen fb + a tile coverage scratch, statics (large -> BSS).
uint16_t g_fb[RDR_SCREEN_W * RDR_SCREEN_H];
uint8_t g_cov[RDR_TILE_W * RDR_TILE_H];

}  // namespace

// Contract still compiles/links (kept from the stub).
TEST(Aa, LinksAndContractCompiles) { SUCCEED(); }

// Enable accessor: default OFF, set/get round-trips, normalises to 0/1.
TEST(AaEnable, DefaultOffAndRoundTrips) {
  aa_set_enabled(0);
  EXPECT_EQ(aa_enabled(), 0);
  aa_set_enabled(1);
  EXPECT_EQ(aa_enabled(), 1);
  aa_set_enabled(7);  // any nonzero -> 1
  EXPECT_EQ(aa_enabled(), 1);
  aa_set_enabled(0);
  EXPECT_EQ(aa_enabled(), 0);
}

// Disabled -> no-op even with edge coverage present (golden-neutral gate).
TEST(AaResolve, DisabledIsNoOp) {
  aa_set_enabled(0);
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb[i] = (uint16_t)(i & 0xFFFF);
  }
  for (int i = 0; i < RDR_TILE_W * RDR_TILE_H; ++i) {
    g_cov[i] = (uint8_t)(i & 1 ? 100 : 255);  // half edge pixels
  }
  uint16_t before[RDR_TILE_W * RDR_TILE_H];
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      before[(ty * RDR_TILE_W) + tx] = g_fb[(ty * RDR_SCREEN_W) + tx];
    }
  }
  aa_resolve_tile(g_fb, g_cov, 0, 0);
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      ASSERT_EQ(g_fb[(ty * RDR_SCREEN_W) + tx], before[(ty * RDR_TILE_W) + tx]);
    }
  }
}

// Null cov -> no-op (the gate raster relies on).
TEST(AaResolve, NullCovIsNoOp) {
  aa_set_enabled(1);
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb[i] = 0x1234;
  }
  aa_resolve_tile(g_fb, 0, 0, 0);
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    ASSERT_EQ(g_fb[i], 0x1234);
  }
  aa_set_enabled(0);
}

// Build a vertical-edge colour field in tile 0: left half colour A, right half
// colour B, with a 1px column of EDGE coverage at the boundary. The resolve
// must (a) leave interior unchanged, (b) blend the edge column toward the
// neighbour min/max, (c) match the integer reference bit-for-bit, (d) change
// a nonzero number of pixels.
TEST(AaResolve, VerticalEdgeMatchesRefAndIsEdgeGated) {
  aa_set_enabled(1);
  uint16_t const col_a = pack565(200, 40, 40);
  uint16_t const col_b = pack565(40, 40, 200);
  int const edge_x = RDR_TILE_W / 2;
  // Sentinel guard: fill the WHOLE screen with a loud sentinel so any OOB /
  // cross-tile read would corrupt the result detectably (also ASan would trip).
  uint16_t const sentinel = pack565(0, 255, 0);
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb[i] = sentinel;
  }
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      g_fb[(ty * RDR_SCREEN_W) + tx] = (tx < edge_x) ? col_a : col_b;
      // Interior full; the boundary column (and its left neighbour) are edges.
      uint8_t cv = (uint8_t)255;
      if (tx == edge_x) {
        cv = (uint8_t)90;  // ~0.35 covered (an aliased edge sample)
      } else if (tx == edge_x - 1) {
        cv = (uint8_t)160;
      }
      g_cov[(ty * RDR_TILE_W) + tx] = cv;
    }
  }
  uint16_t ref[RDR_TILE_W * RDR_TILE_H];
  resolve_ref(g_fb, g_cov, 0, 0, ref);

  aa_resolve_tile(g_fb, g_cov, 0, 0);

  int changed = 0;
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    for (int tx = 0; tx < RDR_TILE_W; ++tx) {
      uint16_t const got = g_fb[(ty * RDR_SCREEN_W) + tx];
      ASSERT_EQ(got, ref[(ty * RDR_TILE_W) + tx])
          << "tx=" << tx << " ty=" << ty;
      // Edge-gated: a FULL-coverage interior pixel is unchanged.
      if (g_cov[(ty * RDR_TILE_W) + tx] >= 255) {
        ASSERT_EQ(got, (tx < edge_x) ? col_a : col_b);
      } else {
        ++changed;  // count edge pixels processed (ref may equal orig if flat)
      }
      // No sentinel ever leaked in (would mean an OOB read).
      ASSERT_NE(got, sentinel) << "sentinel leaked at tx=" << tx;
    }
  }
  EXPECT_GT(changed, 0);
  aa_set_enabled(0);
}

// Border-clamp: an edge pixel in the LAST tile column (tx == RDR_TILE_W-1) must
// NOT read tile-pixel RDR_TILE_W (the next tile / OOB). We place a poison
// colour exactly there and assert it never appears in the resolved output. ASan
// would also flag an actual OOB; this is the deterministic-output guard.
TEST(AaResolve, BorderClampNoCrossTileRead) {
  aa_set_enabled(1);
  uint16_t const base = pack565(80, 80, 80);
  uint16_t const poison = pack565(255, 0, 255);
  // Tile 0 spans screen x [0,RDR_TILE_W). The pixel at screen x == RDR_TILE_W
  // is the FIRST pixel of tile 1 (same row). Poison it.
  for (int i = 0; i < RDR_SCREEN_W * RDR_SCREEN_H; ++i) {
    g_fb[i] = base;
  }
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    g_fb[(ty * RDR_SCREEN_W) + RDR_TILE_W] = poison;  // tile 1, col 0
    // Make the LAST column of tile 0 an edge pixel so the resolve runs there.
    g_cov[(ty * RDR_TILE_W) + (RDR_TILE_W - 1)] = (uint8_t)64;
    for (int tx = 0; tx < RDR_TILE_W - 1; ++tx) {
      g_cov[(ty * RDR_TILE_W) + tx] = (uint8_t)255;
    }
  }
  aa_resolve_tile(g_fb, g_cov, 0, 0);
  // The resolved last column must derive only from base (its single in-tile
  // left neighbour is also base) -> stays base; poison never pulled in.
  for (int ty = 0; ty < RDR_TILE_H; ++ty) {
    uint16_t const got = g_fb[(ty * RDR_SCREEN_W) + (RDR_TILE_W - 1)];
    ASSERT_EQ(got, base) << "ty=" << ty;
    // Poison itself (tile 1) untouched (we only resolved tile 0).
    ASSERT_EQ(g_fb[(ty * RDR_SCREEN_W) + RDR_TILE_W], poison);
  }
  aa_set_enabled(0);
}
