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
// fx_mul: rounded Q16.16 multiply. 64-bit product synthesized from 16x16
// partials (no UMULL on ARMv6-M); host and device are bit-identical.
fx16_16 fx_mul(fx16_16 a, fx16_16 b);
// fx_div: Q16.16 divide a/b. Device routes the integer divide to the SIO
// hardware divider (truncating, toward zero); host uses C integer division,
// which has identical truncating semantics -> bit-identical result.
// Divide-by-zero returns saturated +/-FX_MAX/MIN (caller rejects degenerates).
fx_invw fx_div(fx16_16 a, fx16_16 b);

// ---- conversions (setup only; intention-revealing, not for inner loops) ----
fx16_16 fx_from_int(int32_t n);
int32_t fx_to_int(fx16_16 a);  // truncates toward zero

// ---- mat4 (column-major, Q16.16) -------------------------------------------
void mat4_identity(struct Mat4fx* m);
// mat4_mul: out = a * b (column-major; out may not alias a or b).
void mat4_mul(struct Mat4fx* out, const struct Mat4fx* a,
              const struct Mat4fx* b);
// mat4_mul_vec4: out = m * v (column-major; out may not alias v).
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
