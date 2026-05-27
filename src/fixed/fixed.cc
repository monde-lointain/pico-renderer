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
// Divide: fx_div widens the dividend to 64-bit ((int64)a << 16), so C's '/'
// lowers to __aeabi_ldivmod (a 64/32 divide), NOT the 32-bit single-op path.
// The Pico SDK backs __aeabi_ldivmod with the SIO hardware divider only when
// pico_divider is linked (this lib pulls no such dep; it links via the default
// runtime). Regardless of backing, the divide truncates toward zero, and host
// C '/' truncates identically -> host/device numerics are bit-identical. (An
// explicit async hw_divider_* path only helps overlapped divides; not needed
// here, and would add a hardware/divider.h dependency.)
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
  uint32_t const a_lo = ua & 0xFFFFU;
  uint32_t const a_hi = ua >> 16;
  uint32_t const b_lo = ub & 0xFFFFU;
  uint32_t const b_hi = ub >> 16;

  uint32_t const ll = a_lo * b_lo;  // 16x16 -> <=32 bits (single MULS)
  uint32_t const lh = a_lo * b_hi;  // 16x16
  uint32_t const hl = a_hi * b_lo;  // 16x16
  uint32_t const hh = a_hi * b_hi;  // 16x16

  uint64_t const mid = (uint64_t)lh + (uint64_t)hl;
  uint64_t const mag = (uint64_t)ll + (mid << 16) + ((uint64_t)hh << 32);
  return (sign < 0) ? -(int64_t)mag : (int64_t)mag;
}

fx16_16 fx_mul(fx16_16 a, fx16_16 b) {
  int64_t const p = smul64(a, b);
  // Round half away from zero. Operate on the magnitude with a logical shift
  // (arithmetic '>>' floors, which would skew negatives by one ULP).
  if (p >= 0) {
    return (fx16_16)(((uint64_t)p + FX_HALF) >> FX_FRAC_BITS);
  }
  return (fx16_16)(-(int64_t)(((uint64_t)(-p) + FX_HALF) >> FX_FRAC_BITS));
}

fx_invw fx_div(fx16_16 a, fx16_16 b) {
  if (b == 0) {
    if (a > 0) {
      return INT32_MAX;
    }
    if (a < 0) {
      return INT32_MIN;
    }
    return 0;
  }
  int64_t const num = (int64_t)a << FX_FRAC_BITS;
  // Truncates toward zero. On device this lowers to the SIO divider (AEABI);
  // on host to the same C truncating divide -> bit-identical.
  return (fx_invw)(num / b);
}

fx16_16 fx_from_int(int32_t n) {
  // Shift through unsigned: a signed left-shift that overflows int32 is UB,
  // and |n| >= 2^15 overflows here. The unsigned shift wraps (well-defined);
  // the precondition |n| < 2^15 still applies for a meaningful result.
  return (fx16_16)((uint32_t)n << FX_FRAC_BITS);
}

int32_t fx_to_int(fx16_16 a) {
  // Truncate toward zero (not arithmetic floor). Compute via a 64-bit divide
  // so INT32_MIN is handled without negating it (negating INT32_MIN is UB).
  return (int32_t)((int64_t)a / FX_ONE);
}

void mat4_identity(struct Mat4fx* m) {
  for (int i = 0; i < 16; ++i) {
    m->m[i] = 0;
  }
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
        acc += fx_mul(a->m[(k * 4) + row], b->m[(col * 4) + k]);
      }
      out->m[(col * 4) + row] = acc;
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
      acc += fx_mul(m->m[(k * 4) + row], in[k]);
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
  if (v <= 0) {
    return 0;
  }
  // Compute isqrt of (v << 16) so the result is Q16.16: sqrt(v/2^16)*2^16
  // = sqrt(v*2^16). Use unsigned 64-bit accumulation (shifts/adds; no UMULL).
  uint64_t n = (uint64_t)(uint32_t)v << FX_FRAC_BITS;
  uint64_t res = 0;
  uint64_t bit = (uint64_t)1 << 62;
  while (bit > n) {
    bit >>= 2;
  }
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
  fx16_16 const len2 = vec3_dot(v, v);  // |v|^2 in Q16.16
  if (len2 <= 0) {
    out->x = 0;
    out->y = 0;
    out->z = 0;
    return;
  }
  fx16_16 const len = fx_sqrt(len2);
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
