// demo/main.cc — renderer demo entry (Wave-D.7). Drives the testable terrain
// scene builder (demo_scene.cc) through the rdr façade each frame, presents,
// and advances the DETERMINISTIC scripted camera + panorama scroll by fixed
// integer deltas (so fb_crc reproduces and host==device). BTN_A toggles an
// interactive free-fly camera. On device it logs KEY=VALUE telemetry over
// USB-CDC, including a dedicated scene-build/cmdgen stage counter distinct from
// the geom transform/bin and back-end raster work, then a final RESULT=PASS.
//
// The scene math/geometry lives in demo_scene.cc so the actual demo scene is
// host-testable (see tests/demo) — the C1 black-screen regression escaped
// because the demo's own build path was never exercised on host.
//
// NOT orthodoxy_enforced (app-level carve-out, like src/app). Still C-like.

#include <stddef.h>
#include <stdint.h>

#include "aa/aa.h"  // aa_set_enabled (T4a: coverage AA on)
#include "demo/demo_scene.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"
#include "platform/reset_stub.h"  // hardware watchdog + BOOTSEL reset (robustness)
#include "prof/prof.h"  // T5: granular intra-frame profiler (no-op when PROFILER=0)
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
// frame (dual-core output must be bit-identical to single-core, and the
// integer-only camera/scroll path must reproduce the same sequence).
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

// ---- scene/camera state (deterministic; advanced by INTEGER deltas) --------
static struct DemoCamera s_cam;
static struct DemoScroll s_scroll;

// ---- storage (large; BSS, not stack) ---------------------------------------
static struct Command s_cmd[DEMO_TERRAIN_CMD_CAP];
static struct Frame s_frame;
static struct Framebuffer s_present_fb;

int main(void) {
  plat_init();
  prof_init();  // T5: cache clk_sys Hz AFTER plat_init's 250MHz overclock
  prof_systick_enable();  // T5: enable SysTick on core0 (per-core banked; core1
                          // enables its own in dispatch_core1_main)

  s_frame.fb = s_present_fb.px;
  s_frame.width = SCREEN_W;
  s_frame.height = SCREEN_H;

  demo_camera_init(&s_cam);
  demo_scroll_init(&s_scroll);
  aa_set_enabled(1);  // T4a: coverage AA on for the demo (terrain fog too, set
                      // in the terrain material). Runtime flag (default OFF).

  // Robustness (on-target checklist): arm the hardware watchdog so a hung frame
  // self-recovers (reboots) instead of permanently wedging the device — which
  // otherwise needs a physical BOOTSEL. Kicked once per frame below; ~8.3 s is
  // the RP2040 max load and comfortably exceeds the worst frame (<1 s).
  plat_watchdog_arm(8388);

  uint32_t frame = 0;
  for (;;) {
    plat_watchdog_kick();  // feed the dog; a frame that hangs >8.3 s trips it
                           // and reboots, recovering the device
    // Poll input first so the free-fly toggle/nudge applies this frame. A
    // shutdown request (host window closed) ends the loop cleanly.
    struct Input in;
    bool const alive = plat_poll_input(&in);

    // T5: zero the per-core anchors BEFORE t0 (core1 is parked between frames,
    // so g_prof[1] is quiescent here) — keeps the cheap reset out of PROF_ROOT.
    prof_frame_begin();
    uint32_t const t0 = plat_millis();
    struct DemoTelemetry telem;
    {
      // PROF_ROOT brackets EXACTLY build..present (== the frame_ms span;
      // fb_crc32 below is outside it, as frame_ms already excludes it). Its
      // exclusive time = the unwrapped glue (begin_frame + fill_blits + loop).
      PROF_BLOCK(PROF_ROOT);
      {
        PROF_BLOCK(PROF_CMDGEN);  // single-core front-end scene build
        demo_terrain_build(s_cmd, DEMO_TERRAIN_CMD_CAP, &s_cam, &s_scroll,
                           &telem);
      }
      rdr_begin_frame(&s_frame);
      // T3a/T3b: fill the 2D sky-blit list AFTER begin_frame (which reset
      // blit_count) and BEFORE end_frame runs the blits as the full-frame
      // background: blits[0]=scrolling panorama, blits[1]=cloud band (when the
      // pose shows sky). Deterministic on the scripted path (committed sky
      // table) so device fb_crc == host reference.
      demo_fill_blits(&s_frame, &s_cam);
      {
        PROF_BLOCK(
            PROF_GEOM);  // rdr_submit -> sched_geom -> geom_run (geom-only)
        rdr_submit(&s_frame, s_cmd);
      }
      // PROF_CLEAR_BLIT + PROF_RASTER_DISPATCH live INSIDE rdr_end_frame (the
      // clear + 2D blits + sched_rasterize are one call — see rdr.cc).
      rdr_end_frame(&s_frame);
      {
        PROF_BLOCK(
            PROF_PRESENT);  // serial scanout / upload tax (a watermark lever)
        plat_present(&s_present_fb);
      }
    }
    uint32_t const t1 = plat_millis();

    uint32_t const fb_crc = fb_crc32(s_present_fb.px, SCREEN_W * SCREEN_H);
    // Per-STAGE telemetry: cmdgen (single-core front-end scene build) is called
    // out separately from geom transform/bin (tris=/dropped=) and back-end
    // raster (workers=). cam_mode surfaces the scripted/free-fly toggle.
    // `frame=` is the 0-based scripted-frame index — the alignment key for the
    // host↔device fb_crc cross-check (T0): the firmware does NOT wait for the
    // USB-CDC host to attach, so leading frames are dropped from any capture;
    // matching device `frame=K` against the host reference's `frame=K` makes
    // the comparison robust to that enumeration delay (and to ACM
    // re-enumeration).
    plat_log(
        "frame=%u frame_ms=%u cmdgen_cmds=%u cmdgen_draws=%u cmdgen_tris=%u "
        "tris=%u dropped=%u scroll=%u pano=%u blits=%u cam_mode=%u workers=%u "
        "fb_crc=%08x\n",
        (unsigned)frame, (unsigned)(t1 - t0), (unsigned)telem.cmdgen_commands,
        (unsigned)telem.cmdgen_draws, (unsigned)telem.cmdgen_tris,
        (unsigned)s_frame.geom.tris_total, (unsigned)s_frame.geom.tris_dropped,
        (unsigned)telem.scroll_phase, (unsigned)s_frame.blits[0].scroll_x,
        (unsigned)s_frame.blit_count, (unsigned)s_cam.mode,
        (unsigned)DEMO_RASTER_WORKERS, (unsigned)fb_crc);

    // T5: fold this frame's per-core anchors into the run accumulator and emit
    // a periodic snapshot every PROF_DUMP_EVERY frames. Runs AFTER the frame=
    // log (and after t1), so the multi-line USB burst does not inflate the
    // window.
    prof_frame_end(frame, (uint32_t)(t1 - t0));

    // Advance the DETERMINISTIC per-frame path by integer deltas (no float).
    demo_camera_advance(&s_cam, in.held, in.pressed);
    demo_scroll_advance(&s_scroll);

    ++frame;
    if (!alive) {
      break;
    }
    if (DEMO_FRAMES != 0 && frame >= (uint32_t)DEMO_FRAMES) {
      break;
    }
  }
  // Bounded exit (profiler/test preset). Feed the watchdog before the final
  // report (unkicked, and a multi-line USB burst under PROFILER=1), then return
  // the device to BOOTSEL so the host runner can reflash without a physical
  // button press — and so we never strand in a post-main idle with no watchdog
  // kick. (Host: plat_reset_to_bootsel is a no-op and we return normally.)
  plat_watchdog_kick();
  prof_report_final();  // T5: final averaged prof block at the bounded exit
  plat_log("RESULT=PASS\n");
  plat_reset_to_bootsel();
  return 0;
}
