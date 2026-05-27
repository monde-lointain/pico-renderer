// fixed.h — module interface (Stream B / fixed math). Orthodox C++.
// Q-format fixed-point + vec/matrix math built on the frozen rdr/types.h
// formats (fx16_16 Q16.16, fx12_4 Q12.4, fx_invw 1/w). No behavior in this
// header — see fixed.cc. POD only; pointers not references; plain enum.
#ifndef RDR_FIXED_H
#define RDR_FIXED_H
#include <stdint.h>

#include "rdr/types.h"

// ---- module-local POD ------------------------------------------------------
// 4-component Q16.16 vector. Not a cross-module pipeline type (the pipeline
// carries Vtx/TVtx from rdr/types.h); this is the natural math primitive for
// homogeneous clip-space transform output. If a transformed clip-space vertex
// must cross a module boundary later, that is a contract-delta for the Lead.
struct Vec4fx {
  fx16_16 x, y, z, w;
};

// ---- Q16.16 scalar ops -----------------------------------------------------
// fx_mul: rounded Q16.16 multiply (round half away from zero). The 32x32->64
// product is synthesized from 16x16 partials (no UMULL on ARMv6-M); host and
// device run the same integer path -> bit-identical.
// Precondition: |a*b| in Q16.16 must fit int32 (|result| < 2^15 in real units).
// Out of that domain the >>16 result wraps (well-defined two's-complement, but
// numerically garbage); callers must keep operands in range. Not saturated.
fx16_16 fx_mul(fx16_16 a, fx16_16 b);
// fx_div: divide a/b. The dividend is widened to 64-bit ((int64)a<<16), so C's
// '/' lowers to __aeabi_ldivmod (a 64/32 divide), NOT the 32-bit single-op
// path; the SDK backs __aeabi_ldivmod with the SIO hardware divider only when
// pico_divider is linked. Either way it truncates toward zero, and host C '/'
// truncates identically -> host/device bit-identical numerics.
// Divide-by-zero returns saturated +/-INT32_MAX/MIN (caller rejects
// degenerates); a/0 with a==0 returns 0.
// NOTE: the return type is fx_invw because the primary use is the perspective
// 1/w divide; it is a plain Q16.16 quotient and is reused as such by
// vec3_normalize. (Type tag reflects the dominant caller, not a unit change.)
fx_invw fx_div(fx16_16 a, fx16_16 b);

// ---- conversions (setup only; intention-revealing, not for inner loops) ----
// fx_from_int: precondition |n| < 2^15 (the Q16.16 integer range). The shift
// is done through unsigned so out-of-domain inputs wrap (well-defined) rather
// than invoking signed-overflow UB; the wrapped value is not meaningful.
fx16_16 fx_from_int(int32_t n);
int32_t fx_to_int(fx16_16 a);  // truncates toward zero (defined for INT32_MIN)

// ---- mat4 (column-major, Q16.16) -------------------------------------------
void mat4_identity(struct Mat4fx* m);
// mat4_mul: out = a * b (column-major; out may not alias a or b).
void mat4_mul(struct Mat4fx* out, const struct Mat4fx* a,
              const struct Mat4fx* b);
// mat4_mul_vec4: out = m * v (column-major). out MAY alias v (the input is
// buffered before any store). out must NOT alias m.
void mat4_mul_vec4(struct Vec4fx* out, const struct Mat4fx* m,
                   const struct Vec4fx* v);

// ---- transform / vector math -----------------------------------------------
// mat4_transform_point: out = m * (p, 1) -> homogeneous clip coords (incl w).
void mat4_transform_point(struct Vec4fx* out, const struct Mat4fx* m,
                          const struct Vec3fx* p);
// vec3_dot: Q16.16 dot product (rounded).
fx16_16 vec3_dot(const struct Vec3fx* a, const struct Vec3fx* b);
// vec3_normalize: out = v / |v| (unit vector). Zero-length input -> zero out.
// out may alias v.
void vec3_normalize(struct Vec3fx* out, const struct Vec3fx* v);

#endif  // RDR_FIXED_H
