// fixed.cc — fixed-point math (Stream B). Orthodox C++. Stubs first
// (contract-first); behavior added under orthodox-tdd-cycle.
#include "fixed/fixed.h"

fx16_16 fx_mul(fx16_16 a, fx16_16 b) {
  (void)a;
  (void)b;
  return 0;
}

fx_invw fx_div(fx16_16 a, fx16_16 b) {
  (void)a;
  (void)b;
  return 0;
}

fx16_16 fx_from_int(int32_t n) {
  (void)n;
  return 0;
}

int32_t fx_to_int(fx16_16 a) {
  (void)a;
  return 0;
}

void mat4_identity(struct Mat4fx* m) { (void)m; }

void mat4_mul(struct Mat4fx* out, const struct Mat4fx* a,
              const struct Mat4fx* b) {
  (void)out;
  (void)a;
  (void)b;
}

void mat4_mul_vec4(struct Vec4fx* out, const struct Mat4fx* m,
                   const struct Vec4fx* v) {
  (void)out;
  (void)m;
  (void)v;
}

void mat4_transform_point(struct Vec4fx* out, const struct Mat4fx* m,
                          const struct Vec3fx* p) {
  (void)out;
  (void)m;
  (void)p;
}

fx16_16 vec3_dot(const struct Vec3fx* a, const struct Vec3fx* b) {
  (void)a;
  (void)b;
  return 0;
}

void vec3_normalize(struct Vec3fx* out, const struct Vec3fx* v) {
  (void)out;
  (void)v;
}
