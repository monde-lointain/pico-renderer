// demo_scene_test.cc — C1 demo-scene host guard. gtest.
//
// REGRESSION GUARD: the C1 demo shipped a black screen to the device — the
// camera sat behind the geometry (view translate -18 with a +z-forward clip
// convention), so every source triangle hit the near-reject (clip.w<=0) and
// tris=0 dropped=16. It escaped because the 14 rdr goldens use their OWN
// matrices; the demo's actual build_scene was never exercised on host. This
// test drives the REAL demo scene (demo_scene_build) through the full rdr
// pipeline and asserts the scene actually renders. Deterministic (angle 0).
#include "demo/demo_scene.h"

#include <stdint.h>
#include <string.h>

#include "gfx/framebuffer.h"
#include "gtest/gtest.h"
#include "rdr/config.h"
#include "rdr/frame.h"
#include "rdr/rdr.h"

// Frame is large (pool + bins) -> BSS, not the stack.
static struct Frame g_frame;
static struct Framebuffer g_fb;

// Count framebuffer pixels that differ from the scene's CLEAR color.
static int count_non_clear(const uint16_t* fb, int n, uint16_t clear) {
  int drawn = 0;
  for (int i = 0; i < n; ++i) {
    if (fb[i] != clear) {
      ++drawn;
    }
  }
  return drawn;
}

TEST(DemoSceneGuard, SceneRendersTrisAndPixels) {
  struct Command cmds[DEMO_CMD_CAP];
  uint32_t const n = demo_scene_build(cmds, DEMO_CMD_CAP, 0.0F, 0.0F);
  ASSERT_GT(n, 0U);
  ASSERT_LE(n, (uint32_t)DEMO_CMD_CAP);
  // Last command must be END-terminated. Index via a clamped local so it is
  // provably in [0, DEMO_CMD_CAP) for the static analyzer.
  uint32_t const last = (n - 1) % (uint32_t)DEMO_CMD_CAP;
  ASSERT_EQ(cmds[last].op, (uint8_t)CMD_END);

  g_frame.fb = g_fb.px;
  g_frame.width = RDR_SCREEN_W;
  g_frame.height = RDR_SCREEN_H;
  ASSERT_EQ(rdr_begin_frame(&g_frame), RDR_OK);
  ASSERT_EQ(rdr_submit(&g_frame, cmds), RDR_OK);
  ASSERT_EQ(rdr_end_frame(&g_frame), RDR_OK);

  // The regression that bit us: tris_total must be > 0. The scene is 16 source
  // tris (4 pyramid + 12 cube); back-face cull legitimately removes those
  // facing away, so assert a sensible lower bound, not all 16.
  EXPECT_GE(g_frame.geom.tris_total, 4U)
      << "tris_total=" << g_frame.geom.tris_total
      << " dropped=" << g_frame.geom.tris_dropped
      << " (black-screen regression: camera behind geometry?)";

  // The scene must actually rasterize, not just bin: at least one framebuffer
  // pixel differs from the CLEAR color.
  uint16_t const clear = rgb565(8, 8, 24);
  int const drawn =
      count_non_clear(g_frame.fb, RDR_SCREEN_W * RDR_SCREEN_H, clear);
  EXPECT_GT(drawn, 0) << "no non-clear pixels — scene binned but did not draw";
}
