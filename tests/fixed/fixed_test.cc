// fixed_test.cc — host golden tests for the fixed-point math module.
// Strategy (golden-image-test skill): compare the fixed-point result to a
// float oracle within a stated tolerance. Each nontrivial op had an injected
// error confirmed to fail before trusting green. Inputs are eyeball-verifiable.
#include "fixed/fixed.h"

#include <math.h>
#include <stdint.h>

#include "gtest/gtest.h"

// Q16.16 has 2^-16 resolution. One ULP = 1 in the raw int32 representation.
static const double K_FX_SCALE = 65536.0;

static double fx_to_double(fx16_16 a) { return (double)a / K_FX_SCALE; }
static fx16_16 fx_from_double(double d) {
  return (fx16_16)llround(d * K_FX_SCALE);
}

// ---- conversions -----------------------------------------------------------
TEST(FixedConv, FromInt) {
  EXPECT_EQ(fx_from_int(0), 0);
  EXPECT_EQ(fx_from_int(1), 65536);
  EXPECT_EQ(fx_from_int(-3), -3 * 65536);
}

TEST(FixedConv, ToIntTruncatesTowardZero) {
  EXPECT_EQ(fx_to_int(fx_from_int(5)), 5);
  EXPECT_EQ(fx_to_int(fx_from_double(2.75)), 2);
  EXPECT_EQ(fx_to_int(fx_from_double(-2.75)), -2);  // toward zero, not floor
}

// Edge: fx_to_int(INT32_MIN) must not negate INT32_MIN (would be UB/overflow).
// INT32_MIN = -32768.0 exactly in Q16.16, so the truncated int is -32768.
TEST(FixedConv, ToIntIntMinIsDefined) {
  EXPECT_EQ(fx_to_int(INT32_MIN), -32768);
  EXPECT_EQ(fx_to_int(INT32_MAX), 32767);  // 32767.999.. -> trunc 32767
}

// Edge: fx_from_int at the |n| < 2^15 domain boundary. Inside the domain the
// result is exact; just below the negative edge it stays exact too.
TEST(FixedConv, FromIntDomainBoundary) {
  EXPECT_EQ(fx_from_int(32767), 32767 * 65536);
  EXPECT_EQ(fx_from_int(-32768), INT32_MIN);  // -32768 * 65536 == INT32_MIN
}

// Out-of-domain fx_from_int must be well-defined (unsigned wrap), NOT UB. We
// don't assert a meaningful value -- only that it returns a deterministic
// two's-complement wrap (i.e. matches the documented unsigned-shift semantics).
TEST(FixedConv, FromIntOutOfDomainWrapsDeterministically) {
  EXPECT_EQ(fx_from_int(32768), (fx16_16)((uint32_t)32768 << 16));
  EXPECT_EQ(fx_from_int(-40000), (fx16_16)((uint32_t)(-40000) << 16));
}

// ---- fx_mul (rounded Q16.16) -----------------------------------------------
TEST(FixedMul, ByEye) {
  // 2.0 * 3.5 = 7.0
  EXPECT_EQ(fx_mul(fx_from_double(2.0), fx_from_double(3.5)),
            fx_from_double(7.0));
  // 0.5 * 0.5 = 0.25
  EXPECT_EQ(fx_mul(fx_from_double(0.5), fx_from_double(0.5)),
            fx_from_double(0.25));
}

TEST(FixedMul, SignsAndZero) {
  EXPECT_EQ(fx_mul(fx_from_double(-2.0), fx_from_double(3.0)),
            fx_from_double(-6.0));
  EXPECT_EQ(fx_mul(fx_from_double(-2.0), fx_from_double(-3.0)),
            fx_from_double(6.0));
  EXPECT_EQ(fx_mul(0, fx_from_double(123.5)), 0);
}

TEST(FixedMul, OracleSweepRounded) {
  // Sweep small/fractional pairs; fixed must match the rounded float product
  // to within 1 ULP (rounding, not truncation).
  for (int ai = -200; ai <= 200; ai += 7) {
    for (int bi = -200; bi <= 200; bi += 11) {
      double const a = ai * 0.013;
      double const b = bi * 0.017;
      fx16_16 const got = fx_mul(fx_from_double(a), fx_from_double(b));
      fx16_16 const want = fx_from_double(fx_to_double(fx_from_double(a)) *
                                          fx_to_double(fx_from_double(b)));
      EXPECT_LE(llabs((long long)got - (long long)want), 1)
          << "a=" << a << " b=" << b;
    }
  }
}

// ---- fx_div (SIO divider semantics; truncating toward zero) ----------------
TEST(FixedDiv, ByEye) {
  // 7.0 / 2.0 = 3.5
  EXPECT_EQ(fx_div(fx_from_double(7.0), fx_from_double(2.0)),
            fx_from_double(3.5));
  // 1.0 / 4.0 = 0.25
  EXPECT_EQ(fx_div(fx_from_double(1.0), fx_from_double(4.0)),
            fx_from_double(0.25));
}

TEST(FixedDiv, Signs) {
  EXPECT_EQ(fx_div(fx_from_double(-7.0), fx_from_double(2.0)),
            fx_from_double(-3.5));
  EXPECT_EQ(fx_div(fx_from_double(-7.0), fx_from_double(-2.0)),
            fx_from_double(3.5));
}

TEST(FixedDiv, OracleSweep) {
  // Fixed divide truncates toward zero (matches SIO + C /). Compare to a
  // truncated oracle so host == device == oracle exactly.
  for (int ai = -300; ai <= 300; ai += 9) {
    for (int bi = 1; bi <= 60; bi += 3) {
      double const a = ai * 0.021;
      double const b = bi * 0.037;
      fx16_16 const fa = fx_from_double(a);
      fx16_16 const fb = fx_from_double(b);
      fx_invw const got = fx_div(fa, fb);
      // Oracle: ((int64)fa << 16) / fb, truncating toward zero.
      int64_t const num = (int64_t)fa << 16;
      int32_t const want = (int32_t)(num / fb);
      EXPECT_EQ(got, want) << "a=" << a << " b=" << b;
    }
  }
}

TEST(FixedDiv, ByZeroSaturates) {
  EXPECT_EQ(fx_div(fx_from_double(5.0), 0), INT32_MAX);
  EXPECT_EQ(fx_div(fx_from_double(-5.0), 0), INT32_MIN);
  EXPECT_EQ(fx_div(0, 0), 0);
}

// Overflow domain: fx_mul precondition is |a*b| < 2^15 (real units). A product
// that exceeds the Q16.16 range wraps (two's-complement, well-defined) rather
// than producing UB or silent garbage from an undefined shift. We pin the
// deterministic wrapped value so the documented behavior is tested, not silent.
TEST(FixedMul, OverflowDomainWrapsDeterministically) {
  // 200.0 * 200.0 = 40000.0, which does not fit Q16.16 (max ~ +32767.99998).
  fx16_16 const big = fx_from_int(200);
  int64_t const p = (int64_t)big * (int64_t)big;  // exact 64-bit product
  fx16_16 const want =
      (fx16_16)(((uint64_t)p + (1 << 15)) >> 16);  // round-half-up
  EXPECT_EQ(fx_mul(big, big), want);
}

// Overflow domain: fx_div where the 64-bit numerator/quotient exceeds int32.
// (int64)a<<16 / b can exceed INT32 range; the (fx_invw) cast wraps. Pin the
// deterministic truncated-then-cast value so the behavior is documented/tested.
TEST(FixedDiv, OverflowDomainWrapsDeterministically) {
  fx16_16 const a = fx_from_int(20000);  // large dividend
  fx16_16 const b = fx_from_double(0.001);
  int64_t const num = (int64_t)a << 16;
  fx_invw const want =
      (fx_invw)(num / b);  // truncate toward zero, then cast/wrap
  EXPECT_EQ(fx_div(a, b), want);
}

// ---- mat4 ------------------------------------------------------------------
TEST(Mat4, Identity) {
  struct Mat4fx m;
  mat4_identity(&m);
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      fx16_16 const want = (c == r) ? fx_from_int(1) : 0;
      EXPECT_EQ(m.m[(c * 4) + r], want) << "c=" << c << " r=" << r;
    }
  }
}

TEST(Mat4, MulIdentityIsLeftAndRightNeutral) {
  struct Mat4fx id;
  mat4_identity(&id);
  struct Mat4fx a;
  for (int i = 0; i < 16; ++i) {
    a.m[i] = fx_from_double((i * 0.5) - 3.0);
  }
  struct Mat4fx r;
  mat4_mul(&r, &id, &a);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(r.m[i], a.m[i]) << "left i=" << i;
  }
  mat4_mul(&r, &a, &id);
  for (int i = 0; i < 16; ++i) {
    EXPECT_EQ(r.m[i], a.m[i]) << "right i=" << i;
  }
}

TEST(Mat4, MulMatchesFloatOracle) {
  struct Mat4fx a;
  struct Mat4fx b;
  for (int i = 0; i < 16; ++i) {
    a.m[i] = fx_from_double(((i % 5) * 0.3) - 0.6);
    b.m[i] = fx_from_double(((i % 7) * 0.2) - 0.4);
  }
  struct Mat4fx got;
  mat4_mul(&got, &a, &b);
  // Float oracle: column-major c[col*4+row] = sum_k a[k*4+row]*b[col*4+k].
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      double acc = 0.0;
      for (int k = 0; k < 4; ++k) {
        acc +=
            fx_to_double(a.m[(k * 4) + row]) * fx_to_double(b.m[(col * 4) + k]);
      }
      fx16_16 const want = fx_from_double(acc);
      EXPECT_LE(llabs((long long)got.m[(col * 4) + row] - (long long)want), 4)
          << "col=" << col << " row=" << row;
    }
  }
}

// ---- transform -------------------------------------------------------------
TEST(Transform, TranslateThenProjectW) {
  // Build a matrix that translates by (10,20,30) (column-major: translation in
  // column 3) and verify mat4_transform_point applies the implicit w=1.
  struct Mat4fx m;
  mat4_identity(&m);
  m.m[12] = fx_from_int(10);
  m.m[13] = fx_from_int(20);
  m.m[14] = fx_from_int(30);
  struct Vec3fx const p = {fx_from_int(1), fx_from_int(2), fx_from_int(3)};
  struct Vec4fx out;
  mat4_transform_point(&out, &m, &p);
  EXPECT_EQ(out.x, fx_from_int(11));
  EXPECT_EQ(out.y, fx_from_int(22));
  EXPECT_EQ(out.z, fx_from_int(33));
  EXPECT_EQ(out.w, fx_from_int(1));
}

TEST(Transform, MulVec4MatchesOracle) {
  struct Mat4fx m;
  for (int i = 0; i < 16; ++i) {
    m.m[i] = fx_from_double(((i % 6) * 0.25) - 0.5);
  }
  struct Vec4fx const v = {fx_from_double(1.5), fx_from_double(-2.0),
                           fx_from_double(0.75), fx_from_double(1.0)};
  struct Vec4fx got;
  mat4_mul_vec4(&got, &m, &v);
  double const vv[4] = {fx_to_double(v.x), fx_to_double(v.y), fx_to_double(v.z),
                        fx_to_double(v.w)};
  double oracle[4] = {0, 0, 0, 0};
  for (int row = 0; row < 4; ++row) {
    for (int k = 0; k < 4; ++k) {
      oracle[row] += fx_to_double(m.m[(k * 4) + row]) * vv[k];
    }
  }
  fx16_16 const g[4] = {got.x, got.y, got.z, got.w};
  for (int row = 0; row < 4; ++row) {
    EXPECT_LE(llabs((long long)g[row] - (long long)fx_from_double(oracle[row])),
              4)
        << "row=" << row;
  }
}

// out MAY alias v: passing the same buffer for out and v must give the same
// result as a separate output (the input is buffered before any store).
TEST(Transform, MulVec4AliasSafe) {
  struct Mat4fx m;
  for (int i = 0; i < 16; ++i) {
    m.m[i] = fx_from_double(((i % 6) * 0.25) - 0.5);
  }
  struct Vec4fx const in = {fx_from_double(1.5), fx_from_double(-2.0),
                            fx_from_double(0.75), fx_from_double(1.0)};
  struct Vec4fx sep;
  mat4_mul_vec4(&sep, &m, &in);
  struct Vec4fx aliased = in;
  mat4_mul_vec4(&aliased, &m, &aliased);  // out == v
  EXPECT_EQ(aliased.x, sep.x);
  EXPECT_EQ(aliased.y, sep.y);
  EXPECT_EQ(aliased.z, sep.z);
  EXPECT_EQ(aliased.w, sep.w);
}

// ---- vec3 dot / normalize --------------------------------------------------
TEST(Vec3, Dot) {
  struct Vec3fx const a = {fx_from_double(1.0), fx_from_double(2.0),
                           fx_from_double(3.0)};
  struct Vec3fx const b = {fx_from_double(4.0), fx_from_double(-5.0),
                           fx_from_double(6.0)};
  // 4 - 10 + 18 = 12
  fx16_16 const got = vec3_dot(&a, &b);
  EXPECT_LE(llabs((long long)got - (long long)fx_from_double(12.0)), 2);
}

TEST(Vec3, NormalizeUnitLength) {
  struct Vec3fx const v = {fx_from_double(3.0), fx_from_double(4.0),
                           fx_from_double(0.0)};
  struct Vec3fx n;
  vec3_normalize(&n, &v);
  // Expect (0.6, 0.8, 0.0); generous tolerance (sqrt + 2 divides).
  EXPECT_NEAR(fx_to_double(n.x), 0.6, 0.01);
  EXPECT_NEAR(fx_to_double(n.y), 0.8, 0.01);
  EXPECT_NEAR(fx_to_double(n.z), 0.0, 0.01);
  // Length ~= 1.
  double const len = sqrt((fx_to_double(n.x) * fx_to_double(n.x)) +
                          (fx_to_double(n.y) * fx_to_double(n.y)) +
                          (fx_to_double(n.z) * fx_to_double(n.z)));
  EXPECT_NEAR(len, 1.0, 0.01);
}

TEST(Vec3, NormalizeZeroIsZero) {
  struct Vec3fx const z = {0, 0, 0};
  struct Vec3fx n;
  vec3_normalize(&n, &z);
  EXPECT_EQ(n.x, 0);
  EXPECT_EQ(n.y, 0);
  EXPECT_EQ(n.z, 0);
}
