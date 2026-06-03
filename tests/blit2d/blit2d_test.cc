// blit2d_test.cc — host golden tests for the 2D sky background blits.
// References: docs/superpowers/specs design (panorama / sky background),
// N64 GBI GPACK_RGBA5551 (R[15:11] G[10:6] B[5:1] A[0]), gfx/framebuffer.h
// rgb565() packer. Orthodox C++.
#include "blit2d/blit2d.h"

#include <stdint.h>

#include "gtest/gtest.h"

// Contract smoke: the stub library links and dispatch rejects unknown modes.
TEST(Blit2d, ContractLinks) {
  EXPECT_EQ(blit2d_decode_i8(nullptr, 0, 0, 0), 0);
}
