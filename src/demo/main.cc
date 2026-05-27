// demo/main.cc — renderer demo (C1). A port of the gldemo flat/shaded scene
// (a rotating pyramid + a rotating flat-colored cube) driven through the rdr
// façade. Builds a command stream each frame, runs begin/submit/end, presents,
// and advances the animation. On device it logs KEY=VALUE telemetry over
// USB-CDC (frame_ms=, tris=) and a final RESULT=PASS.
//
// NOT orthodoxy_enforced (app-level carve-out, like src/app). Still C-like.
//
// FOLLOW-ON HOOK: the gldemo TEXTURED_CUBE path (CI4 + TLUT, no lighting) is a
// separate stream. The cube draw is factored behind DEMO_TEXTURED_CUBE so it
// drops in later without restructuring; today only the flat path is built.

#include <math.h>
#include <stdint.h>
#include <string.h>

#include "cmd/cmd.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"
#include "rdr/frame.h"
#include "rdr/rdr.h"

#ifndef DEMO_FRAMES
#define DEMO_FRAMES                                        \
  0 /* 0 = run forever (device); >0 = bounded (host smoke) \
     */
#endif

// ---- Q16.16 matrix builders (demo-local; fixed.cc has no proj/lookAt) ------
#define FXONE (1 << 16)
static fx16_16 fxd(double v) { return (fx16_16)lrint(v * 65536.0); }

static void m_identity(struct Mat4fx* m) {
  memset(m->m, 0, sizeof m->m);
  m->m[0] = FXONE;
  m->m[5] = FXONE;
  m->m[10] = FXONE;
  m->m[15] = FXONE;
}

// Column-major translate.
static void m_translate(struct Mat4fx* m, double x, double y, double z) {
  m_identity(m);
  m->m[12] = fxd(x);
  m->m[13] = fxd(y);
  m->m[14] = fxd(z);
}

// Rotation about a unit axis (degrees), column-major (Rodrigues).
static void m_rotate(struct Mat4fx* m, double deg, double ax, double ay,
                     double az) {
  double const len = sqrt((ax * ax) + (ay * ay) + (az * az));
  if (len > 0.0) {
    ax /= len;
    ay /= len;
    az /= len;
  }
  double const r = deg * 3.14159265358979323846 / 180.0;
  double const c = cos(r);
  double const s = sin(r);
  double const t = 1.0 - c;
  m_identity(m);
  // col0
  m->m[0] = fxd((t * ax * ax) + c);
  m->m[1] = fxd((t * ax * ay) + (s * az));
  m->m[2] = fxd((t * ax * az) - (s * ay));
  // col1
  m->m[4] = fxd((t * ax * ay) - (s * az));
  m->m[5] = fxd((t * ay * ay) + c);
  m->m[6] = fxd((t * ay * az) + (s * ax));
  // col2
  m->m[8] = fxd((t * ax * az) + (s * ay));
  m->m[9] = fxd((t * ay * az) - (s * ax));
  m->m[10] = fxd((t * az * az) + c);
}

// Perspective (column-major, maps z to [0,1], w=z). fov in degrees, aspect 1.
static void m_perspective(struct Mat4fx* m, double fov_deg, double aspect,
                          double n, double f) {
  double const fscale =
      1.0 / tan(fov_deg * 0.5 * 3.14159265358979323846 / 180.0);
  memset(m->m, 0, sizeof m->m);
  m->m[0] = fxd(fscale / aspect);
  m->m[5] = fxd(fscale);
  m->m[10] = fxd(f / (f - n));
  m->m[11] = fxd(1.0);  // w = z
  m->m[14] = fxd(-(f * n) / (f - n));
}

// Compose out = a * b (apply b first). Wraps fixed.cc mat4_mul.
static void m_mul(struct Mat4fx* out, const struct Mat4fx* a,
                  const struct Mat4fx* b) {
  mat4_mul(out, a, b);
}

// ---- scene geometry (ported from gldemo geometry.c, scaled for Q16.16) -----
// The N64 demo uses positions in [-50,50] with camera at z=300. We scale the
// world down by SCENE_SCALE so all transformed coords stay well within the
// Q16.16 integer range (|v| < 2^15) — geometry/colors are otherwise verbatim.
#define SCENE_SCALE 0.06 /* 50 -> 3, 300 -> 18, 75 -> 4.5 */

// 12 pyramid verts (4 triangular faces), provoking-vertex color is v0 (apex).
static const struct Vtx PYRAMID_VTX[12] = {
    {{0, 3, 0}, {0, 0}, {{255, 0, 0, 255}}},
    {{-3, -3, 3}, {0, 0}, {{0, 255, 0, 255}}},
    {{3, -3, 3}, {0, 0}, {{0, 0, 255, 255}}},
    {{0, 3, 0}, {0, 0}, {{255, 0, 0, 255}}},
    {{3, -3, 3}, {0, 0}, {{0, 0, 255, 255}}},
    {{3, -3, -3}, {0, 0}, {{0, 255, 0, 255}}},
    {{0, 3, 0}, {0, 0}, {{255, 0, 0, 255}}},
    {{3, -3, -3}, {0, 0}, {{0, 255, 0, 255}}},
    {{-3, -3, -3}, {0, 0}, {{0, 0, 255, 255}}},
    {{0, 3, 0}, {0, 0}, {{255, 0, 0, 255}}},
    {{-3, -3, -3}, {0, 0}, {{0, 0, 255, 255}}},
    {{-3, -3, 3}, {0, 0}, {{0, 255, 0, 255}}},
};
static const uint16_t PYRAMID_IDX[12] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

// 24 cube verts (4 per face, solid face colors), 6 faces * 2 tris.
static const struct Vtx CUBE_VTX[24] = {
    {{3, 3, -3}, {0, 0}, {{0, 255, 0, 255}}}, /* Top - green */
    {{-3, 3, -3}, {0, 0}, {{0, 255, 0, 255}}},
    {{-3, 3, 3}, {0, 0}, {{0, 255, 0, 255}}},
    {{3, 3, 3}, {0, 0}, {{0, 255, 0, 255}}},
    {{3, -3, 3}, {0, 0}, {{255, 128, 0, 255}}}, /* Bottom - orange */
    {{-3, -3, 3}, {0, 0}, {{255, 128, 0, 255}}},
    {{-3, -3, -3}, {0, 0}, {{255, 128, 0, 255}}},
    {{3, -3, -3}, {0, 0}, {{255, 128, 0, 255}}},
    {{3, 3, 3}, {0, 0}, {{255, 0, 0, 255}}}, /* Front - red */
    {{-3, 3, 3}, {0, 0}, {{255, 0, 0, 255}}},
    {{-3, -3, 3}, {0, 0}, {{255, 0, 0, 255}}},
    {{3, -3, 3}, {0, 0}, {{255, 0, 0, 255}}},
    {{3, -3, -3}, {0, 0}, {{255, 255, 0, 255}}}, /* Back - yellow */
    {{-3, -3, -3}, {0, 0}, {{255, 255, 0, 255}}},
    {{-3, 3, -3}, {0, 0}, {{255, 255, 0, 255}}},
    {{3, 3, -3}, {0, 0}, {{255, 255, 0, 255}}},
    {{-3, 3, 3}, {0, 0}, {{0, 0, 255, 255}}}, /* Left - blue */
    {{-3, 3, -3}, {0, 0}, {{0, 0, 255, 255}}},
    {{-3, -3, -3}, {0, 0}, {{0, 0, 255, 255}}},
    {{-3, -3, 3}, {0, 0}, {{0, 0, 255, 255}}},
    {{3, 3, -3}, {0, 0}, {{255, 0, 255, 255}}}, /* Right - violet */
    {{3, 3, 3}, {0, 0}, {{255, 0, 255, 255}}},
    {{3, -3, 3}, {0, 0}, {{255, 0, 255, 255}}},
    {{3, -3, -3}, {0, 0}, {{255, 0, 255, 255}}},
};
static const uint16_t CUBE_IDX[36] = {
    0,  1,  2,  0,  2,  3,  4,  5,  6,  4,  6,  7,  8,  9,  10, 8,  10, 11,
    12, 13, 14, 12, 14, 15, 16, 17, 18, 16, 18, 19, 20, 21, 22, 20, 22, 23};

// ---- per-frame command buffer ----------------------------------------------
enum { DEMO_CMD_CAP = 64 };
static struct Command s_cmd[DEMO_CMD_CAP];
static struct CmdBuf s_cb = {s_cmd, 0, DEMO_CMD_CAP, 0};

// Persistent matrices (must outlive cb_walk: Command holds const Mat4fx*).
static struct Mat4fx s_proj;
static struct Mat4fx s_view;
static struct Mat4fx s_view_proj;  // projection * view (loaded as projection)
static struct Mat4fx s_pyramid_mv;
static struct Mat4fx s_cube_mv;

static struct RenderState s_state;  // flat, opaque, back-face cull

static float s_pyr_angle = 0.0F;
static float s_cube_angle = 0.0F;

static void push(uint8_t op, const struct Command* fill) {
  struct Command c = *fill;
  c.op = op;
  cb_push(&s_cb, &c);
}

// guLookAt for an eye on +z looking down -z at the origin with +y up reduces to
// a translate by -eye on z (no rotation). Camera at z = +18 (scaled 300).
static void build_view(void) { m_translate(&s_view, 0.0, 0.0, -18.0); }

static void build_scene(void) {
  cb_reset(&s_cb);

  // Projection (45 deg, square aspect for 240x240) composed with the view.
  m_perspective(&s_proj, 45.0, 1.0, 0.6, 60.0);
  build_view();
  m_mul(&s_view_proj, &s_proj, &s_view);

  // CLEAR to dark blue, then load the composed view-projection as PROJECTION.
  struct Command c;
  memset(&c, 0, sizeof c);
  c.u.clear.color = rgb565(8, 8, 24);
  push(CMD_CLEAR, &c);

  memset(&c, 0, sizeof c);
  c.u.set_matrix.target = MTX_PROJECTION;
  c.u.set_matrix.push = 0;
  c.u.set_matrix.mat = &s_view_proj;
  push(CMD_SET_MATRIX, &c);

  // Flat-shaded, opaque, back-face cull (matches gldemo flat path).
  memset(&s_state, 0, sizeof s_state);
  s_state.zmode = ZMODE_OPAQUE;
  s_state.cull = CULL_BACK;
  s_state.lit = 0;
  memset(&c, 0, sizeof c);
  c.u.set_material.state = &s_state;
  push(CMD_SET_MATERIAL, &c);

  // ---- pyramid: rotate about Y, translate left (-4.5 scaled) ----
  {
    struct Mat4fx rot;
    struct Mat4fx trans;
    m_rotate(&rot, s_pyr_angle, 0.0, 1.0, 0.0);
    m_translate(&trans, -4.5, 0.0, 0.0);
    m_mul(&s_pyramid_mv, &trans, &rot);
    memset(&c, 0, sizeof c);
    c.u.set_matrix.target = MTX_MODELVIEW;
    c.u.set_matrix.push = 1;
    c.u.set_matrix.mat = &s_pyramid_mv;
    push(CMD_SET_MATRIX, &c);
    memset(&c, 0, sizeof c);
    c.u.load_verts.ptr = PYRAMID_VTX;
    c.u.load_verts.count = 12;
    push(CMD_LOAD_VERTS, &c);
    memset(&c, 0, sizeof c);
    c.u.draw_tris.idx = PYRAMID_IDX;
    c.u.draw_tris.tri_count = 4;
    push(CMD_DRAW_TRIS, &c);
    push(CMD_POP_MATRIX, &c);
  }

  // ---- cube: rotate about (1,1,1), translate right (+4.5, z -3) ----
  {
    struct Mat4fx rot;
    struct Mat4fx trans;
    m_rotate(&rot, s_cube_angle, 1.0, 1.0, 1.0);
    m_translate(&trans, 4.5, 0.0, -3.0);
    m_mul(&s_cube_mv, &trans, &rot);
    memset(&c, 0, sizeof c);
    c.u.set_matrix.target = MTX_MODELVIEW;
    c.u.set_matrix.push = 1;
    c.u.set_matrix.mat = &s_cube_mv;
    push(CMD_SET_MATRIX, &c);
#ifdef DEMO_TEXTURED_CUBE
    /* FOLLOW-ON: load CI4 texture + TLUT, set decal combiner, draw textured
     * mesh. Wired by the texturing stream; intentionally absent today. */
#else
    memset(&c, 0, sizeof c);
    c.u.load_verts.ptr = CUBE_VTX;
    c.u.load_verts.count = 24;
    push(CMD_LOAD_VERTS, &c);
    memset(&c, 0, sizeof c);
    c.u.draw_tris.idx = CUBE_IDX;
    c.u.draw_tris.tri_count = 12;
    push(CMD_DRAW_TRIS, &c);
#endif
    push(CMD_POP_MATRIX, &c);
  }

  // Terminate the stream (required: streams MUST be END-terminated).
  memset(&c, 0, sizeof c);
  push(CMD_END, &c);
}

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

// ---- frame storage (large; BSS, not stack) ---------------------------------
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
    build_scene();
    rdr_begin_frame(&s_frame);
    rdr_submit(&s_frame, s_cmd);
    rdr_end_frame(&s_frame);
    plat_present(&s_present_fb);
    uint32_t const t1 = plat_millis();

    plat_log("frame_ms=%u tris=%u dropped=%u\n", (unsigned)(t1 - t0),
             (unsigned)s_frame.geom.tris_total,
             (unsigned)s_frame.geom.tris_dropped);

    update_animation();
    ++frame;
    if (DEMO_FRAMES != 0 && frame >= (uint32_t)DEMO_FRAMES) {
      break;
    }
  }
  plat_log("RESULT=PASS\n");
  return 0;
}
