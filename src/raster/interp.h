#ifndef RENDERER_RASTER_INTERP_H
#define RENDERER_RASTER_INTERP_H

#include <stdint.h>

// Portable inline-control. always_inline/noinline are GCC/Clang attributes
// (host clang, macOS appleclang, ARM gcc all honor them); MSVC — the host
// Windows CI build — rejects __attribute__, so they no-op there. These attrs
// only steer CODEGEN (per-pixel steppers must fold into the SRAM raster loop on
// device; per-tri setup must stay flash-resident), which matters on the ARM
// target; on a compiler without them the optimizer decides, fine for the
// host correctness build. (Same #ifdef-guard rationale as rdr/sram.h.)
// NOLINTBEGIN(cppcoreguidelines-macro-usage) — portability shim, not
// constexpr-able
#ifdef __GNUC__
#define INTERP_ALWAYS_INLINE __attribute__((always_inline))
#define INTERP_NOINLINE __attribute__((noinline))
#else
#define INTERP_ALWAYS_INLINE
#define INTERP_NOINLINE
#endif
// NOLINTEND(cppcoreguidelines-macro-usage)

// interp.h — screen-affine interpolant stepper (T5 L1: two-stage exact-integer
// DDA). Replaces the per-pixel divide
//     value = (int32_t)(num / area2)
// — where  num = w0*Q0 + w1*Q1 + w2*Q2  is screen-LINEAR (the edge functions
// w_i are linear in screen x,y, the Q_i are per-vertex constants) and the
// divisor area2 is the per-triangle 2*area, > 0 after winding-normalize — with
// adds-only stepping that reproduces the C trunc-toward-zero divide
// BIT-FOR-BIT. This is golden-neutral: the rasterizer's output (and the dual==
// serial + host==device fb_crc gate) is unchanged; only the per-pixel cost
// drops (6 ÷ + ~9 lmul -> adds).
//
// LICENSE TO STEP (Abrash, Graphics Programming Black Book ch66): "1/z *does*
// vary linearly [in screenspace]" — and so do s/z, t/z and screen-affine SHADE.
// inv_w, u_iw, v_iw, Gouraud and fog are exactly these screen-linear
// quantities, so each is an exact linear DDA in (x,y): num steps by a
// per-triangle integer constant per pixel, and value = num/area2 follows.
//
// METHOD = Abrash's all-integer error-term DDA, the verbatim structure of his
// ch56 SetUpEdge/StepEdge destination-X stepper:
//     SetUpEdge:  IntStep = W / H;  AdjUp = W % H;  AdjDown = H;  ErrTerm = 0;
//     StepEdge:   X += IntStep;  if ((ErrTerm += AdjUp) > 0) { X += dir;
//                                                              ErrTerm -=
//                                                              AdjDown; }
// i.e. do the divide ONCE at setup (quotient = whole step, remainder = AdjUp),
// then step with adds + a carry. ch39: "Floating point is accurate but not
// precise; integer calculations, properly performed, are" — Abrash uses the
// INTEGER DDA for the destination X precisely because it must be EXACT (gaps
// between adjacent polygons otherwise); our golden/dual==serial gate is the
// same "must be exact" contract, so we use the integer DDA, not his faster
// fixed-point SourceX accumulate (ch57: fine for the texture source, where a
// 1-px shift is "insignificant" — NOT for us, where any LSB moves the fb_crc).
//
// ONE DELIBERATE DEVIATION — phase. Abrash's ErrTerm starts at 0 and tests `>
// 0`, a phase tuned to ROUND for gap-free edge placement (his goal). OUR goal
// is to reproduce the EXISTING per-pixel divide `(int32_t)(num/area2)`, which
// TRUNCATES toward zero (C `/`). Since area2 > 0 (winding-normalized) we step a
// FLOORED divmod instead — remainder kept in [0, area2) so there is exactly one
// conditional carry per step and no sign-flip at zero — and apply the trunc
// correction at READOUT: the numerator only goes negative at a USED (inside)
// pixel for the u_iw/v_iw texcoord interpolants (inv_w, Gouraud, fog are >= 0
// there), and for those trunc = floor + 1 when the quotient is negative and the
// remainder is non-zero. That recovers C's truncation exactly ->
// golden-neutral.
//
// Orthodox C++: POD struct + interp_* free functions, static inline so the
// stepper folds into the __not_in_flash_func raster loops (no per-pixel call;
// Abrash ch57 §6.1). State (qf, rf and the steps) is 32-bit — qf is the
// interpolated value (fits the int32 the divide produced), rf is in [0, area2)
// (< the int32 area2) — so there is no per-pixel 64-bit accumulator.
struct Interp {
  int32_t qf;      // floored quotient at the current pixel (value, pre-trunc)
  int32_t rf;      // floored remainder at the current pixel, in [0, area2)
  int32_t qf_row;  // row-start floored quotient (advanced once per scanline)
  int32_t rf_row;  // row-start floored remainder, in [0, area2)
  int32_t qx;      // per-pixel-x floored step quotient (= floor(dx / area2))
  int32_t rx;      // per-pixel-x floored step remainder, in [0, area2)
  int32_t qy;      // per-scanline-y floored step quotient (= floor(dy / area2))
  int32_t ry;      // per-scanline-y floored step remainder, in [0, area2)
  int32_t area2;   // divisor, > 0
};

// Floored divmod: quotient floor(a / b) with b > 0; remainder (in [0, b))
// written via *r. a may be negative. The quotient and remainder both fit int32
// for our operands (quotient = an interpolated value or its per-pixel delta;
// remainder < area2). Setup frequency (per triangle), not per pixel.
static inline int32_t interp_floor_divmod(int64_t a, int32_t b, int32_t* r) {
  int64_t q = a / (int64_t)b;          // C division: truncates toward zero
  int64_t rem = a - (q * (int64_t)b);  // remainder, sign of a
  if (rem < 0) {                       // bias to floored: remainder in [0, b)
    rem += (int64_t)b;
    --q;
  }
  *r = (int32_t)rem;
  return (int32_t)q;
}

// Seed the stepper for a triangle. num_origin = numerator at the row-origin
// pixel center (caller's choice of origin x at scanline 0); dx = exact
// per-pixel-x numerator delta; dy = exact per-scanline-y numerator delta; area2
// > 0. interp_begin_row must be called before each scanline's inner loop.
static inline void interp_init(struct Interp* p, int64_t num_origin, int64_t dx,
                               int64_t dy, int32_t area2) {
  p->area2 = area2;
  p->qf_row = interp_floor_divmod(num_origin, area2, &p->rf_row);
  p->qx = interp_floor_divmod(dx, area2, &p->rx);
  p->qy = interp_floor_divmod(dy, area2, &p->ry);
  p->qf = p->qf_row;  // defined pre-begin_row; begin_row reloads at row start
  p->rf = p->rf_row;
}

// Start a scanline: load the inner accumulator from the row-start, then advance
// the row-start to the NEXT scanline (one floored +y step). Call once per sy,
// before the inner x loop. rf_row and ry are both in [0, area2) -> at most one
// carry. always_inline: per-scanline hot path — MUST fold into the SRAM raster
// loop at every -O level (at -Os the optimizer otherwise emits a flash call +
// SRAM->flash veneer per call, which both wrecks the loop and makes the -Os
// profiler unrepresentative of the shipped -O3 build).
static inline INTERP_ALWAYS_INLINE void interp_begin_row(struct Interp* p) {
  p->qf = p->qf_row;
  p->rf = p->rf_row;
  p->qf_row += p->qy;
  p->rf_row += p->ry;
  if (p->rf_row >= p->area2) {
    p->rf_row -= p->area2;
    ++p->qf_row;
  }
}

// Advance the inner accumulator one pixel in +x (one floored +x step). rf and
// rx are both in [0, area2) -> at most one carry. MUST be called for EVERY
// pixel in the bounding-box row, unconditionally — the accumulator must stay in
// lockstep with sx even across outside / z-rejected / saturated pixels (the L1
// desync invariant). always_inline: the per-PIXEL hot path — must fold into the
// SRAM loop at every -O level (see interp_begin_row).
static inline INTERP_ALWAYS_INLINE void interp_step_x(struct Interp* p) {
  p->qf += p->qx;
  p->rf += p->rx;
  if (p->rf >= p->area2) {
    p->rf -= p->area2;
    ++p->qf;
  }
}

// The interpolated value at the current pixel: trunc(num / area2),
// bit-identical to (int32_t)(num / area2) for area2 > 0. num = qf*area2 + rf
// with rf in [0, area2), so num < 0 iff qf < 0; C truncates toward zero, which
// is floor + 1 for a negative non-exact quotient. always_inline: per-PIXEL hot
// path — must fold into the SRAM loop at every -O level (see interp_begin_row).
static inline INTERP_ALWAYS_INLINE int32_t
interp_value(const struct Interp* p) {
  int32_t v = p->qf;
  if (p->qf < 0 && p->rf != 0) {
    ++v;
  }
  return v;
}

#endif  // RENDERER_RASTER_INTERP_H
