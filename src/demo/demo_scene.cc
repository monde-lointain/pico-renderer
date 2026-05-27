// demo/demo_scene.cc — C1 demo scene builder (gldemo flat pyramid + cube).
// Orthodox C++ (NOT orthodoxy_enforced; app-level carve-out, still C-like).
//
// Ported from gldemo render.c/geometry.c. Positions are scaled by SCENE_SCALE
// so all transformed coords stay within the Q16.16 integer range (|v| < 2^15);
// colors/winding are otherwise verbatim. Camera convention is pinned to the
// renderer's +z-FORWARD clip convention (clip.w = +view_z; see m_perspective
// m[11]=1.0): the camera sits at world -Z looking toward +Z, so world-origin
// geometry lands at POSITIVE view-z inside the [near,far] frustum. (A camera at
// +Z with a -Z translate would put the scene behind the near plane -> every
// vertex near-rejected -> black screen; that was the C1 device regression.)

#include "demo/demo_scene.h"

#include <math.h>
#include <string.h>

#include "cmd/cmd.h"
#include "fixed/fixed.h"
#include "gfx/framebuffer.h"

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

static void m_translate(struct Mat4fx* m, double x, double y, double z) {
  m_identity(m);
  m->m[12] = fxd(x);
  m->m[13] = fxd(y);
  m->m[14] = fxd(z);
}

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
  m->m[0] = fxd((t * ax * ax) + c);
  m->m[1] = fxd((t * ax * ay) + (s * az));
  m->m[2] = fxd((t * ax * az) - (s * ay));
  m->m[4] = fxd((t * ax * ay) - (s * az));
  m->m[5] = fxd((t * ay * ay) + c);
  m->m[6] = fxd((t * ay * az) + (s * ax));
  m->m[8] = fxd((t * ax * az) + (s * ay));
  m->m[9] = fxd((t * ay * az) - (s * ax));
  m->m[10] = fxd((t * az * az) + c);
}

// Perspective (column-major, maps z to [0,1], w=z => clip.w = +view_z). fov in
// degrees, square aspect.
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

// ---- scene geometry (gldemo geometry.c, scaled SCENE_SCALE for Q16.16) -----
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

// ---- module-local matrix storage (referenced by the built Command stream) --
static struct Mat4fx s_proj;
static struct Mat4fx s_view;
static struct Mat4fx s_view_proj;  // projection * view (loaded as PROJECTION)
static struct Mat4fx s_pyramid_mv;
static struct Mat4fx s_cube_mv;
static struct RenderState s_state;  // flat, opaque, back-face cull
static struct CmdBuf s_cb;

static void push(uint8_t op, const struct Command* fill) {
  struct Command c = *fill;
  c.op = op;
  cb_push(&s_cb, &c);
}

// Real lookAt (column-major), matched to the renderer's +z-FORWARD clip
// convention: view-z(p) = dot(forward, p - eye), forward = normalize(center -
// eye), so world points IN FRONT of the camera get POSITIVE view-z (the
// perspective slot m[11]=1 => clip.w = +view_z). A bare +z translate would put
// the eye on the -Z side (180deg orbit from gldemo) and show the away-faces;
// the real lookAt places the eye on gldemo's side (eye +Z looking at origin),
// so the +Z world faces (gldemo front, e.g. cube front = red) are nearest and
// visible as a SOLID silhouette. Basis (s,u,f) is right-handed orthonormal
// (det +1, a rotation, NOT a mirror) so it does not disturb det_sign / cull.
static void m_lookat(struct Mat4fx* m, double ex, double ey, double ez,
                     double cx, double cy, double cz, double ux, double uy,
                     double uz) {
  // forward f = normalize(center - eye)  (view-z axis; +z toward the scene)
  double fx = cx - ex;
  double fy = cy - ey;
  double fz = cz - ez;
  double const flen = sqrt((fx * fx) + (fy * fy) + (fz * fz));
  if (flen > 0.0) {
    fx /= flen;
    fy /= flen;
    fz /= flen;
  }
  // s = normalize(f x up)  (view-x axis)
  double sx = (fy * uz) - (fz * uy);
  double sy = (fz * ux) - (fx * uz);
  double sz = (fx * uy) - (fy * ux);
  double const slen = sqrt((sx * sx) + (sy * sy) + (sz * sz));
  if (slen > 0.0) {
    sx /= slen;
    sy /= slen;
    sz /= slen;
  }
  // u = s x f  (view-y axis; already unit)
  double const vx = (sy * fz) - (sz * fy);
  double const vy = (sz * fx) - (sx * fz);
  double const vz = (sx * fy) - (sy * fx);
  // Rows of R are (s, u, f); translation is -R*eye. Column-major store.
  m_identity(m);
  m->m[0] = fxd(sx);
  m->m[4] = fxd(sy);
  m->m[8] = fxd(sz);
  m->m[1] = fxd(vx);
  m->m[5] = fxd(vy);
  m->m[9] = fxd(vz);
  m->m[2] = fxd(fx);
  m->m[6] = fxd(fy);
  m->m[10] = fxd(fz);
  m->m[12] = fxd(-((sx * ex) + (sy * ey) + (sz * ez)));
  m->m[13] = fxd(-((vx * ex) + (vy * ey) + (vz * ez)));
  m->m[14] = fxd(-((fx * ex) + (fy * ey) + (fz * ez)));
}

// gldemo camera: eye at +Z (z=18 scaled from 300) looking at the origin, up +Y.
static void build_view(void) {
  m_lookat(&s_view, 0.0, 0.0, 18.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0);
}

uint32_t demo_scene_build(struct Command* buf, uint32_t cap, float pyr_angle,
                          float cube_angle) {
  s_cb.buf = buf;
  s_cb.cap = cap;
  cb_reset(&s_cb);

  m_perspective(&s_proj, 45.0, 1.0, 0.6, 60.0);
  build_view();
  mat4_mul(&s_view_proj, &s_proj, &s_view);

  struct Command c;
  memset(&c, 0, sizeof c);
  c.u.clear.color = rgb565(8, 8, 24);  // dark blue
  push(CMD_CLEAR, &c);

  memset(&c, 0, sizeof c);
  c.u.set_matrix.target = MTX_PROJECTION;
  c.u.set_matrix.push = 0;
  c.u.set_matrix.mat = &s_view_proj;
  push(CMD_SET_MATRIX, &c);

  memset(&s_state, 0, sizeof s_state);
  s_state.zmode = ZMODE_OPAQUE;
  s_state.cull = CULL_BACK;
  s_state.lit = 0;
  memset(&c, 0, sizeof c);
  c.u.set_material.state = &s_state;
  push(CMD_SET_MATERIAL, &c);

  // ---- pyramid: rotate about Y, translate left ----
  {
    struct Mat4fx rot;
    struct Mat4fx trans;
    m_rotate(&rot, (double)pyr_angle, 0.0, 1.0, 0.0);
    m_translate(&trans, -4.5, 0.0, 0.0);
    mat4_mul(&s_pyramid_mv, &trans, &rot);
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

  // ---- cube: rotate about (1,1,1), translate right ----
  {
    struct Mat4fx rot;
    struct Mat4fx trans;
    m_rotate(&rot, (double)cube_angle, 1.0, 1.0, 1.0);
    m_translate(&trans, 4.5, 0.0, -3.0);
    mat4_mul(&s_cube_mv, &trans, &rot);
    memset(&c, 0, sizeof c);
    c.u.set_matrix.target = MTX_MODELVIEW;
    c.u.set_matrix.push = 1;
    c.u.set_matrix.mat = &s_cube_mv;
    push(CMD_SET_MATRIX, &c);
#ifdef DEMO_TEXTURED_CUBE
    /* FOLLOW-ON: CI4 texture + TLUT + decal combiner + textured mesh. */
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

  memset(&c, 0, sizeof c);
  push(CMD_END, &c);  // streams MUST be END-terminated
  return s_cb.count;
}
