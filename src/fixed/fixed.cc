// fixed.cc — fixed-point math (Stream B). Orthodox C++.
//
// Q-format: fx16_16 is Q16.16 (16 integer + 16 fractional bits). All math is
// integer-only; floats appear ONLY in the host test oracle, never here.
//
// Multiply discipline (ARMv6-M / Cortex-M0+ has MULS 32x32->32 only, no UMULL):
// a full 32x32->64 product is synthesized from 16x16 partials, each of which is
// a single MULS. The host build runs the SAME integer code path, so host and
// device are bit-identical.
//
// Divide: on device, the Pico SDK's AEABI support maps C's '/' and '%' onto
// the SIO hardware integer divider (~8 cyc, per-core; truncates toward zero --
// see RP2040 datasheet 2.3.1.5). On host, C's '/' truncates identically. So
// expressing the divide as a plain integer '/' uses the SIO divider on device
// WITHOUT an explicit hardware/divider.h dependency, and is bit-identical to
// the host path. (An explicit async hw_divider_* path only helps overlapped
// divides; this single divide does not need it.)
#include "fixed/fixed.h"

#include <stdint.h>

#define FX_FRAC_BITS 16
#define FX_ONE (1 << FX_FRAC_BITS)
#define FX_HALF (1 << (FX_FRAC_BITS - 1))

// ---- no-UMULL 32x32 -> 64 signed product -----------------------------------
// Decompose |a|,|b| into 16-bit halves; each cross-product is a 16x16 MULS that
// fits 32 bits. Reassemble into a 64-bit magnitude, then re-apply the sign.
// (uint64_t shifts/adds are cheap; only the multiplies must avoid UMULL, which
// they do by construction since every operand is <= 0xFFFF.)
static int64_t smul64(int32_t a, int32_t b) {
  int32_t sign = 1;
  uint32_t ua = (uint32_t)a;
  uint32_t ub = (uint32_t)b;
  if (a < 0) {
    ua = (uint32_t)(-(int64_t)a);
    sign = -sign;
  }
  if (b < 0) {
    ub = (uint32_t)(-(int64_t)b);
    sign = -sign;
  }
  uint32_t a_lo = ua & 0xFFFFu;
  uint32_t a_hi = ua >> 16;
  uint32_t b_lo = ub & 0xFFFFu;
  uint32_t b_hi = ub >> 16;

  uint32_t ll = a_lo * b_lo;  // 16x16 -> <=32 bits (single MULS)
  uint32_t lh = a_lo * b_hi;  // 16x16
  uint32_t hl = a_hi * b_lo;  // 16x16
  uint32_t hh = a_hi * b_hi;  // 16x16

  uint64_t mid = (uint64_t)lh + (uint64_t)hl;
  uint64_t mag = (uint64_t)ll + (mid << 16) + ((uint64_t)hh << 32);
  return (sign < 0) ? -(int64_t)mag : (int64_t)mag;
}

fx16_16 fx_mul(fx16_16 a, fx16_16 b) {
  int64_t p = smul64(a, b);
  // Round half away from zero. Operate on the magnitude with a logical shift
  // (arithmetic '>>' floors, which would skew negatives by one ULP).
  if (p >= 0) {
    return (fx16_16)(((uint64_t)p + FX_HALF) >> FX_FRAC_BITS);
  }
  return (fx16_16)(-(int64_t)(((uint64_t)(-p) + FX_HALF) >> FX_FRAC_BITS));
}

fx_invw fx_div(fx16_16 a, fx16_16 b) {
  if (b == 0) {
    if (a > 0) return INT32_MAX;
    if (a < 0) return INT32_MIN;
    return 0;
  }
  int64_t num = (int64_t)a << FX_FRAC_BITS;
  // Truncates toward zero. On device this lowers to the SIO divider (AEABI);
  // on host to the same C truncating divide -> bit-identical.
  return (fx_invw)(num / b);
}

fx16_16 fx_from_int(int32_t n) { return (fx16_16)(n << FX_FRAC_BITS); }

int32_t fx_to_int(fx16_16 a) {
  // Truncate toward zero (not arithmetic floor).
  if (a >= 0) return a >> FX_FRAC_BITS;
  return -((-a) >> FX_FRAC_BITS);
}

void mat4_identity(struct Mat4fx* m) {
  for (int i = 0; i < 16; ++i) m->m[i] = 0;
  m->m[0] = FX_ONE;
  m->m[5] = FX_ONE;
  m->m[10] = FX_ONE;
  m->m[15] = FX_ONE;
}

void mat4_mul(struct Mat4fx* out, const struct Mat4fx* a,
              const struct Mat4fx* b) {
  // Column-major: out[col*4+row] = sum_k a[k*4+row] * b[col*4+k].
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      fx16_16 acc = 0;
      for (int k = 0; k < 4; ++k) {
        acc += fx_mul(a->m[k * 4 + row], b->m[col * 4 + k]);
      }
      out->m[col * 4 + row] = acc;
    }
  }
}

void mat4_mul_vec4(struct Vec4fx* out, const struct Mat4fx* m,
                   const struct Vec4fx* v) {
  // Column-major: out[row] = sum_k m[k*4+row] * v[k].
  fx16_16 in[4];
  in[0] = v->x;
  in[1] = v->y;
  in[2] = v->z;
  in[3] = v->w;
  fx16_16 r[4];
  for (int row = 0; row < 4; ++row) {
    fx16_16 acc = 0;
    for (int k = 0; k < 4; ++k) {
      acc += fx_mul(m->m[k * 4 + row], in[k]);
    }
    r[row] = acc;
  }
  out->x = r[0];
  out->y = r[1];
  out->z = r[2];
  out->w = r[3];
}

void mat4_transform_point(struct Vec4fx* out, const struct Mat4fx* m,
                          const struct Vec3fx* p) {
  struct Vec4fx v;
  v.x = p->x;
  v.y = p->y;
  v.z = p->z;
  v.w = FX_ONE;  // implicit homogeneous w = 1
  mat4_mul_vec4(out, m, &v);
}

fx16_16 vec3_dot(const struct Vec3fx* a, const struct Vec3fx* b) {
  return fx_mul(a->x, b->x) + fx_mul(a->y, b->y) + fx_mul(a->z, b->z);
}

// ---- fixed-point integer square root (Q16.16) ------------------------------
// Returns sqrt(v) for v in Q16.16. Uses the bit-by-bit restoring algorithm on
// the underlying integer with a 16-bit fractional shift; integer-only.
static fx16_16 fx_sqrt(fx16_16 v) {
  if (v <= 0) return 0;
  // Compute isqrt of (v << 16) so the result is Q16.16: sqrt(v/2^16)*2^16
  // = sqrt(v*2^16). Use unsigned 64-bit accumulation (shifts/adds; no UMULL).
  uint64_t n = (uint64_t)(uint32_t)v << FX_FRAC_BITS;
  uint64_t res = 0;
  uint64_t bit = (uint64_t)1 << 62;
  while (bit > n) bit >>= 2;
  while (bit != 0) {
    if (n >= res + bit) {
      n -= res + bit;
      res = (res >> 1) + bit;
    } else {
      res >>= 1;
    }
    bit >>= 2;
  }
  return (fx16_16)res;
}

void vec3_normalize(struct Vec3fx* out, const struct Vec3fx* v) {
  fx16_16 len2 = vec3_dot(v, v);  // |v|^2 in Q16.16
  if (len2 <= 0) {
    out->x = 0;
    out->y = 0;
    out->z = 0;
    return;
  }
  fx16_16 len = fx_sqrt(len2);
  if (len == 0) {
    out->x = 0;
    out->y = 0;
    out->z = 0;
    return;
  }
  out->x = (fx16_16)fx_div(v->x, len);
  out->y = (fx16_16)fx_div(v->y, len);
  out->z = (fx16_16)fx_div(v->z, len);
}
