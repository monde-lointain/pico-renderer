// shade.cc — color combiner (modulate preset + general single-cycle mux).
// out = (A-B)*C + D evaluated per channel with N64 RDP arithmetic
// (angrylion combiner.c): out8 = clamp(((a-b)*c + (d<<8) + 0x80) >> 8) on 8-bit
// operands. Inputs are selected from the combiner mux ids in CombinerState.
// texel/shade/PRIM/ENV/result are packed RGB565. See shade.h.
#include "shade/shade.h"

#include "gfx/framebuffer.h"  // rgb565()

// 565 -> 8-bit per channel, bit-replication (matches oracle_unpack565 /
// gfx/framebuffer.h layout: R[15:11] G[10:5] B[4:0]).
static void unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// One channel of the N64 combiner: (a-b)*c + d, scaled by /256, rounded,
// clamped to [0,255]. a,b,c,d are 8-bit.
static uint8_t combine_chan(int32_t a, int32_t b, int32_t c, int32_t d) {
  int32_t out = ((a - b) * c) + (d << 8) + 0x80;
  out >>= 8;
  if (out < 0) {
    out = 0;
  }
  if (out > 255) {
    out = 255;
  }
  return (uint8_t)out;
}

// Resolve a combiner mux id to its 8-bit RGB channels for this pixel.
static void select_input(uint8_t id, const struct RenderState* st,
                         const uint8_t* texel, const uint8_t* shade, uint8_t* r,
                         uint8_t* g, uint8_t* b) {
  switch (id) {
    case CC_TEXEL0:
      *r = texel[0];
      *g = texel[1];
      *b = texel[2];
      return;
    case CC_SHADE:
      *r = shade[0];
      *g = shade[1];
      *b = shade[2];
      return;
    case CC_PRIMITIVE:
      unpack565(st->prim_color, r, g, b);
      return;
    case CC_ENVIRONMENT:
      unpack565(st->env_color, r, g, b);
      return;
    case CC_ONE:
      *r = *g = *b = 255;
      return;
    case CC_COMBINED:  // single-cycle: COMBINED feeds back as 0
    case CC_ZERO:
    default:
      *r = *g = *b = 0;
      return;
  }
}

uint16_t shade_pixel(const struct RenderState* st, uint16_t texel,
                     uint16_t shade, uint8_t* keep) {
  uint8_t t[3];
  uint8_t s[3];
  unpack565(texel, &t[0], &t[1], &t[2]);
  unpack565(shade, &s[0], &s[1], &s[2]);

  // MODULATE preset overrides the mux with TEXEL0*SHADE; CUSTOM uses a/b/c/d.
  uint8_t ia = CC_TEXEL0;
  uint8_t ib = CC_ZERO;
  uint8_t ic = CC_SHADE;
  uint8_t id = CC_ZERO;
  if (st->combiner.mode == COMBINE_CUSTOM) {
    ia = st->combiner.a;
    ib = st->combiner.b;
    ic = st->combiner.c;
    id = st->combiner.d;
  }

  uint8_t ar;
  uint8_t ag;
  uint8_t ab;
  uint8_t br;
  uint8_t bg;
  uint8_t bb;
  uint8_t cr;
  uint8_t cg;
  uint8_t cb;
  uint8_t dr;
  uint8_t dg;
  uint8_t db;
  select_input(ia, st, t, s, &ar, &ag, &ab);
  select_input(ib, st, t, s, &br, &bg, &bb);
  select_input(ic, st, t, s, &cr, &cg, &cb);
  select_input(id, st, t, s, &dr, &dg, &db);

  uint8_t const or8 = combine_chan(ar, br, cr, dr);
  uint8_t const og8 = combine_chan(ag, bg, cg, dg);
  uint8_t const ob8 = combine_chan(ab, bb, cb, db);

  // v1: no alpha channel in 565 -> always keep (alpha-compare is W-later).
  if (keep) {
    *keep = 1;
  }
  return rgb565(or8, og8, ob8);
}
