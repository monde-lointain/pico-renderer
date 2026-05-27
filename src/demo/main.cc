// demo/main.cc — renderer demo entry (C1). Drives the testable scene builder
// (demo_scene.cc) through the rdr façade each frame, presents, and advances the
// animation. On device it logs KEY=VALUE telemetry over USB-CDC (frame_ms=,
// tris=, dropped=) and a final RESULT=PASS. The scene math/geometry lives in
// demo_scene.cc so the actual demo scene is host-testable (see tests/demo).
//
// NOT orthodoxy_enforced (app-level carve-out, like src/app). Still C-like.

#include <stddef.h>
#include <stdint.h>

#include "demo/demo_scene.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"
#include "rdr/frame.h"  // struct Frame, RDR_NUM_RASTER_WORKERS
#include "rdr/rdr.h"

#ifndef DEMO_FRAMES
/* 0 = run forever (device); >0 = bounded (host smoke). */
#define DEMO_FRAMES 0
#endif

// Active raster-worker count for the telemetry line: the firmware dual-core
// dispatch fans across both cores; host/SDL serial uses one.
#if defined(PICO_ON_DEVICE) && PICO_ON_DEVICE
#define DEMO_RASTER_WORKERS RDR_NUM_RASTER_WORKERS
#else
#define DEMO_RASTER_WORKERS 1
#endif

// CRC32 (IEEE, reflected) over the presented framebuffer — the on-target
// determinism assertion compares this against the host serial CRC for the same
// scene/angle (dual-core output must be bit-identical to single-core).
static uint32_t fb_crc32(const uint16_t* fb, int n) {
  const uint8_t* p = (const uint8_t*)fb;
  size_t const len = (size_t)n * sizeof(uint16_t);
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
    for (int k = 0; k < 8; ++k) {
      uint32_t const mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

// ---- animation state -------------------------------------------------------
static float s_pyr_angle = 0.0F;
static float s_cube_angle = 0.0F;

static void update_animation(void) {
  s_pyr_angle += 12.0F / 60.0F;
  if (s_pyr_angle >= 360.0F) {
    s_pyr_angle -= 360.0F;
  }
  s_cube_angle -= 9.0F / 60.0F;
  if (s_cube_angle <= -360.0F) {
    s_cube_angle += 360.0F;
  }
}

// ---- storage (large; BSS, not stack) ---------------------------------------
static struct Command s_cmd[DEMO_CMD_CAP];
static struct Frame s_frame;
static struct Framebuffer s_present_fb;

int main(void) {
  plat_init();

  s_frame.fb = s_present_fb.px;
  s_frame.width = SCREEN_W;
  s_frame.height = SCREEN_H;

  uint32_t frame = 0;
  for (;;) {
    uint32_t const t0 = plat_millis();
    demo_scene_build(s_cmd, DEMO_CMD_CAP, s_pyr_angle, s_cube_angle);
    rdr_begin_frame(&s_frame);
    rdr_submit(&s_frame, s_cmd);
    rdr_end_frame(&s_frame);
    plat_present(&s_present_fb);
    uint32_t const t1 = plat_millis();

    uint32_t const fb_crc = fb_crc32(s_present_fb.px, SCREEN_W * SCREEN_H);
    plat_log("frame_ms=%u tris=%u dropped=%u workers=%u fb_crc=%08x\n",
             (unsigned)(t1 - t0), (unsigned)s_frame.geom.tris_total,
             (unsigned)s_frame.geom.tris_dropped, (unsigned)DEMO_RASTER_WORKERS,
             (unsigned)fb_crc);

    update_animation();
    ++frame;
    if (DEMO_FRAMES != 0 && frame >= (uint32_t)DEMO_FRAMES) {
      break;
    }
  }
  plat_log("RESULT=PASS\n");
  return 0;
}
