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

// R.4 — 2-cycle detail multitexture. Cycle 1 runs `combiner` over the inputs
// (CC_COMBINED=0, as in single-cycle); cycle 2 runs `combiner2` with
// CC_COMBINED fed the cycle-1 RGB output. Per-channel math is the SAME
// (A-B)*C+D as shade_pixel (combine_chan) -> host<->device bit-identical.
// shade_pixel (1-cycle) is UNCHANGED above; this is an additive path.

// Resolve a mux id to its 8-bit RGB, EXTENDED for the 2-cycle path: TEXEL1 is a
// distinct sampled texel, and CC_COMBINED selects the supplied cycle-1 RGB
// (`comb`; pass {0,0,0} in cycle 1, matching single-cycle's COMBINED=0).
static void select_input2(uint8_t id, const struct RenderState* st,
                          const uint8_t* texel0, const uint8_t* texel1,
                          const uint8_t* shade, const uint8_t* comb, uint8_t* r,
                          uint8_t* g, uint8_t* b) {
  switch (id) {
    case CC_COMBINED:
      *r = comb[0];
      *g = comb[1];
      *b = comb[2];
      return;
    case CC_TEXEL0:
      *r = texel0[0];
      *g = texel0[1];
      *b = texel0[2];
      return;
    case CC_TEXEL1:
      *r = texel1[0];
      *g = texel1[1];
      *b = texel1[2];
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
    case CC_ZERO:
    default:
      *r = *g = *b = 0;
      return;
  }
}

// Run ONE combiner cycle over the inputs, writing the RGB result to out[3].
// MODULATE preset = A=a-arm(default TEXEL0),B=ZERO,C=c-arm(default
// SHADE),D=ZERO; CUSTOM uses a/b/c/d verbatim. `comb` is the previous-cycle RGB
// (CC_COMBINED source). Same per-channel (A-B)*C+D as shade_pixel
// (combine_chan).
static void run_cycle(const struct CombinerState* cs,
                      const struct RenderState* st, const uint8_t* texel0,
                      const uint8_t* texel1, const uint8_t* shade,
                      const uint8_t* comb, uint8_t* out) {
  uint8_t ia = CC_TEXEL0;
  uint8_t ib = CC_ZERO;
  uint8_t ic = CC_SHADE;
  uint8_t id = CC_ZERO;
  if (cs->mode == COMBINE_CUSTOM) {
    ia = cs->a;
    ib = cs->b;
    ic = cs->c;
    id = cs->d;
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
  select_input2(ia, st, texel0, texel1, shade, comb, &ar, &ag, &ab);
  select_input2(ib, st, texel0, texel1, shade, comb, &br, &bg, &bb);
  select_input2(ic, st, texel0, texel1, shade, comb, &cr, &cg, &cb);
  select_input2(id, st, texel0, texel1, shade, comb, &dr, &dg, &db);
  out[0] = combine_chan(ar, br, cr, dr);
  out[1] = combine_chan(ag, bg, cg, dg);
  out[2] = combine_chan(ab, bb, cb, db);
}

uint16_t shade_pixel2(const struct RenderState* st, uint16_t texel0,
                      uint16_t texel1, uint16_t shade, uint8_t* keep) {
  uint8_t t0[3];
  uint8_t t1[3];
  uint8_t s[3];
  unpack565(texel0, &t0[0], &t0[1], &t0[2]);
  unpack565(texel1, &t1[0], &t1[1], &t1[2]);
  unpack565(shade, &s[0], &s[1], &s[2]);

  // Cycle 1: CC_COMBINED feeds 0 (no previous output), exactly like the
  // single-cycle path.
  uint8_t const zero[3] = {0, 0, 0};
  uint8_t cyc1[3];
  run_cycle(&st->combiner, st, t0, t1, s, zero, cyc1);

  // Cycle 2: CC_COMBINED feeds the cycle-1 RGB output (feed-forward).
  uint8_t cyc2[3];
  run_cycle(&st->combiner2, st, t0, t1, s, cyc1, cyc2);

  // 565 carries no alpha -> always keep (consistent with shade_pixel).
  if (keep) {
    *keep = 1;
  }
  return rgb565(cyc2[0], cyc2[1], cyc2[2]);
}
