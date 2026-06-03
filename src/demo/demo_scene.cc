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
#include "demo/camera_path_gen.h"  // committed Q16.16 scripted V*P table
#include "fixed/fixed.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"  // enum Button (free-fly toggle/nudge)

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

// ===========================================================================
// Wave-D.7 terrain scene (real-density geometry + deterministic camera)
// ===========================================================================
//
// DETERMINISM DISCIPLINE: floats appear ONLY in one-time setup — the keyframe
// table (demo_camera_init) and the per-frame VIEW/MVP bake (a pure function of
// the integer camera pose). The per-frame camera/scroll ADVANCE is integer-
// only (demo_camera_advance / demo_scroll_advance) so fb_crc reproduces and
// host==device.

// ---- terrain layout (pre-scaled world INTEGER model coords) ----------------
// The placeholder grid is a GROUND height-field in the XZ plane (Y up), like
// the N64 terrain demo: the camera flies above and looks down/forward, so the
// field fills the frame (the "near-terrain fill" worst case). Y carries a small
// per- cell height; X,Z span [-HALF,+HALF]. World half-extent matches the
// pre-scaled N64 patch scale (~18 units); the camera (N64 eye ~36 units
// up/back) keeps the whole field in front of the near plane for the entire
// path. Model coords are int16 (Vtx.pos); modelview is identity (verts authored
// directly in pre-scaled world units). The REAL baked mesh from D.1 replaces
// demo_terrain_geometry at T0 — the rest of the build is geometry-agnostic, so
// KEEP THIS SEAM CLEAN.
#define TERRAIN_HALF 18  /* world half-extent in X and Z (pre-scaled units) */
#define TERRAIN_BASE_Y 0 /* ground plane Y */

// Distinct debug color per mesh cell. A simple hashed palette spreads adjacent
// cells across hue so a one-blob / wrong-winding render is obvious. (NOT the
// N64 flat gray — that would blind the orientation check, per the D.7 brief.)
static void cell_debug_color(int cx, int cy, uint8_t* rgb) {
  // 3-channel saw waves with coprime strides -> many distinct buckets.
  int const h = ((cx * 5) + (cy * 3));
  rgb[0] = (uint8_t)(64 + ((h * 37) & 0xBF));
  rgb[1] = (uint8_t)(64 + (((cy * 53) + 17) & 0xBF));
  rgb[2] = (uint8_t)(64 + (((cx * 61) + 31) & 0xBF));
}

// Procedural placeholder height (integer): a fixed deterministic bump so
// adjacent cells differ in Y (visible relief) without crossing near/far. Pure
// integer; one-time at geometry build.
static int terrain_height(int cx, int cy) {
  int const h = ((((cx * 7) + (cy * 11)) % 5) - 2);  // [-2,2]
  return TERRAIN_BASE_Y + h;
}

uint32_t demo_terrain_geometry(struct Vtx* verts, uint16_t* idx) {
  int const cols = DEMO_TERRAIN_COLS;
  int const rows = DEMO_TERRAIN_ROWS;
  int const vx = cols + 1;
  // Vertex grid in the XZ plane; gx -> X, gy -> Z. Y is the height field.
  for (int gy = 0; gy <= rows; ++gy) {
    for (int gx = 0; gx <= cols; ++gx) {
      int const vi = (gy * vx) + gx;
      int const wx = -TERRAIN_HALF + ((2 * TERRAIN_HALF * gx) / cols);
      int const wz = -TERRAIN_HALF + ((2 * TERRAIN_HALF * gy) / rows);
      struct Vtx* v = &verts[vi];
      memset(v, 0, sizeof *v);
      v->pos[0] = (int16_t)wx;
      v->pos[1] = (int16_t)terrain_height(gx, gy);
      v->pos[2] = (int16_t)wz;
      uint8_t rgb[3];
      cell_debug_color(gx, gy, rgb);
      v->c.rgba[0] = rgb[0];
      v->c.rgba[1] = rgb[1];
      v->c.rgba[2] = rgb[2];
      v->c.rgba[3] = 255;
    }
  }
  // Two tris per quad, wound CCW as seen from ABOVE (+Y, the camera side) so
  // CULL_BACK keeps the upward faces. The provoking vertex (v0) is the cell
  // color. In screen space a top-down view of a CCW-from-above ground quad is
  // CW; the cull contract folds winding sense via the modelview det sign, so
  // this is the natural up-facing order.
  int o = 0;
  for (int gy = 0; gy < rows; ++gy) {
    for (int gx = 0; gx < cols; ++gx) {
      uint16_t const a = (uint16_t)((gy * vx) + gx);
      uint16_t const b = (uint16_t)((gy * vx) + gx + 1);
      uint16_t const c2 = (uint16_t)(((gy + 1) * vx) + gx);
      uint16_t const d = (uint16_t)(((gy + 1) * vx) + gx + 1);
      // Tri 1: a, b, c  | Tri 2: b, d, c  (CCW from +Y; provoking v0 = a / b).
      idx[o++] = a;
      idx[o++] = b;
      idx[o++] = c2;
      idx[o++] = b;
      idx[o++] = d;
      idx[o++] = c2;
    }
  }
  return (uint32_t)(vx * (rows + 1));
}

uint32_t demo_tree_geometry(struct Vtx* verts, uint16_t* idx) {
  // Tree billboard quads: upright planes STANDING on the ground (extending in
  // +Y), scattered over the field at distinct positions, each a DISTINCT bright
  // debug sprite color (so a sprite-id transform/winding bug is catchable). The
  // quad spans X (width) x Y (height); both faces drawn (CULL_BACK keeps the
  // one toward the camera). Two tris per quad. NOT view-aligned (true
  // billboarding needs per-frame float; a static upright quad keeps the
  // per-frame path float-free) — fine for the placeholder.
  int o = 0;
  for (int t = 0; t < DEMO_TREE_COUNT; ++t) {
    // Deterministic scatter inside the field (XZ ground positions).
    int const cx = -12 + ((t * 7) % 25);
    int const cz = -12 + ((t * 11) % 25);
    int const hw = 2;  // billboard half-width (world units, along X)
    int const ht = 5;  // billboard height (world units, along +Y)
    // Distinct bright sprite color per sprite-id.
    uint8_t const r = (uint8_t)(128 + ((t * 43) & 0x7F));
    uint8_t const g = (uint8_t)(128 + ((t * 29) & 0x7F));
    uint8_t const bcol = (uint8_t)(128 + ((t * 71) & 0x7F));
    int const base = t * 4;
    // Corners: 0 bl, 1 br, 2 tl, 3 tr (Y up, X across; Z fixed at cz).
    int const corner[4][2] = {{-hw, 0}, {hw, 0}, {-hw, ht}, {hw, ht}};
    for (int k = 0; k < 4; ++k) {
      struct Vtx* v = &verts[base + k];
      memset(v, 0, sizeof *v);
      v->pos[0] = (int16_t)(cx + corner[k][0]);
      v->pos[1] = (int16_t)(TERRAIN_BASE_Y + corner[k][1]);
      v->pos[2] = (int16_t)cz;
      v->c.rgba[0] = r;
      v->c.rgba[1] = g;
      v->c.rgba[2] = bcol;
      v->c.rgba[3] = 255;
    }
    uint16_t const a = (uint16_t)(base + 0);
    uint16_t const b = (uint16_t)(base + 1);
    uint16_t const c2 = (uint16_t)(base + 2);
    uint16_t const d = (uint16_t)(base + 3);
    // Front face winding toward -Z (a,c,b / b,c,d) AND the mirror (a,b,c /
    // b,d,c) is NOT added — CULL_BACK drops whichever side faces away. Provide
    // the -Z-facing order; the upright quad is visible from the +Z camera
    // hemisphere.
    idx[o++] = a;
    idx[o++] = c2;
    idx[o++] = b;
    idx[o++] = b;
    idx[o++] = c2;
    idx[o++] = d;
  }
  return (uint32_t)(DEMO_TREE_COUNT * 4);
}

// ---- module-local terrain storage (referenced by the built stream) ---------
static struct Vtx s_terrain_vtx[DEMO_TERRAIN_VERTS];
static uint16_t s_terrain_idx[DEMO_TERRAIN_IDX];
static struct Vtx s_tree_vtx[DEMO_TREE_VERTS];
static uint16_t s_tree_idx[DEMO_TREE_IDX];
static int s_terrain_geom_built;   // built once (geometry is static)
static uint32_t s_terrain_vcount;  // verts emitted by demo_terrain_geometry
static uint32_t s_tree_vcount;     // verts emitted by demo_tree_geometry
static struct Mat4fx s_terrain_proj;
static struct Mat4fx s_terrain_view;
static struct Mat4fx s_freefly_vp;     // free-fly only: projection * view
static struct Mat4fx s_terrain_model;  // identity (verts authored in world)
static struct RenderState s_terrain_state;

static void terrain_geom_build_once(void) {
  if (s_terrain_geom_built) {
    return;
  }
  // Capture the RETURNED vert counts (not the macro) so a T0 swap to D.1's
  // real mesh whose vert count differs from DEMO_TERRAIN_VERTS can't silently
  // mismatch the LOAD_VERTS window.
  s_terrain_vcount = demo_terrain_geometry(s_terrain_vtx, s_terrain_idx);
  s_tree_vcount = demo_tree_geometry(s_tree_vtx, s_tree_idx);
  s_terrain_geom_built = 1;
}

// ---- scripted camera (deterministic; FLOAT-FREE per-frame path) ------------
// Q5/TW-05 (Lead decision): the scripted path's V*P matrices are baked OFFLINE
// to a committed Q16.16 table (src/demo/camera_path_gen.h, g_scripted_mvp) by
// src/demo/gen_camera_path.py; the per-frame path here just integer-indexes
// that table (g_scripted_mvp[frame % SCRIPTED_FRAME_COUNT]). Host and device
// read the SAME committed bytes -> bit-identical fb_crc. There is NO on-device
// float on the scripted path (the old keyframe trig + per-frame double mat-mul
// were deleted). The keyframe poses + interpolation schedule live in the
// generator, which mirrors this file's enums; the table is the source of truth.
//
// The DemoCamera eye/look fields are retained only as the FREE-FLY handoff
// state (free-fly is the sanctioned on-device-float exception);
// demo_camera_init seeds them with the LOCKED N64 start pose as committed
// Q16.16 CONSTANTS (no runtime float). 3210/-3330/7330 * 0.005, Y negated (N64
// Y-down -> our Y-up), computed once via asset_convert.to_q16_16 (matches
// gen_camera_path frame 0).
#define N64_START_EYE_X 1051853 /* fx16_16(3210*0.005)  ~16.05 */
#define N64_START_EYE_Y 1091174 /* fx16_16(3330*0.005)  ~16.65 (up) */
#define N64_START_EYE_Z 2401894 /* fx16_16(7330*0.005)  ~36.65 */

void demo_camera_init(struct DemoCamera* cam) {
  memset(cam, 0, sizeof *cam);
  cam->mode = DEMO_CAM_SCRIPTED;
  cam->frame = 0;
  cam->eye[0] = N64_START_EYE_X;
  cam->eye[1] = N64_START_EYE_Y;
  cam->eye[2] = N64_START_EYE_Z;
  cam->look[0] = 0;
  cam->look[1] = 0;
  cam->look[2] = 0;
  cam->yaw_q16 = 0;
}

// Free-fly integer deltas (Q16.16 per frame). Conservative so the path stays
// in range; the toggle is the demo's interactive probe, not a determinism
// surface (its input is integer button state).
#define FREEFLY_STEP (1 << 14) /* 0.25 world units / frame */

void demo_camera_advance(struct DemoCamera* cam, uint32_t held,
                         uint32_t pressed) {
  // BTN_A edge toggles scripted <-> free-fly.
  if (pressed & (uint32_t)BTN_A) {
    cam->mode = (cam->mode == DEMO_CAM_SCRIPTED) ? (uint8_t)DEMO_CAM_FREEFLY
                                                 : (uint8_t)DEMO_CAM_SCRIPTED;
  }
  if (cam->mode == DEMO_CAM_FREEFLY) {
    // Integer eye nudge from the D-pad/face buttons (no float).
    if (held & (uint32_t)BTN_LEFT) {
      cam->eye[0] -= FREEFLY_STEP;
    }
    if (held & (uint32_t)BTN_RIGHT) {
      cam->eye[0] += FREEFLY_STEP;
    }
    if (held & (uint32_t)BTN_UP) {
      cam->eye[1] += FREEFLY_STEP;
    }
    if (held & (uint32_t)BTN_DOWN) {
      cam->eye[1] -= FREEFLY_STEP;
    }
    if (held & (uint32_t)BTN_X) {
      cam->eye[2] += FREEFLY_STEP;  // dolly out
    }
    if (held & (uint32_t)BTN_Y) {
      cam->eye[2] -= FREEFLY_STEP;  // dolly in
    }
    return;  // free-fly does not advance the scripted frame
  }
  // Scripted: advance one integer frame. The rendered V*P is read from the
  // committed table at demo_terrain_build time (g_scripted_mvp[frame % N]) —
  // no pose math, no float, on this path.
  ++cam->frame;
}

// ---- panorama / cloud scroll (deterministic integer phase) -----------------
void demo_scroll_init(struct DemoScroll* s) { s->phase = 0; }

void demo_scroll_advance(struct DemoScroll* s) {
  s->phase =
      (s->phase + (uint32_t)DEMO_SCROLL_DELTA) % (uint32_t)DEMO_SCROLL_PERIOD;
}

// ---- terrain scene build ---------------------------------------------------
uint32_t demo_terrain_build(struct Command* buf, uint32_t cap,
                            const struct DemoCamera* cam,
                            const struct DemoScroll* scroll,
                            struct DemoTelemetry* telem) {
  terrain_geom_build_once();
  s_cb.buf = buf;
  s_cb.cap = cap;
  cb_reset(&s_cb);

  memset(telem, 0, sizeof *telem);
  telem->scroll_phase = scroll->phase;
  m_identity(&s_terrain_model);  // verts authored directly in pre-scaled world

  // ---- V*P selection: scripted = committed table (FLOAT-FREE); free-fly =
  //      on-device float bake (the sanctioned interactive exception) ---------
  const struct Mat4fx* vp;
  if (cam->mode == DEMO_CAM_SCRIPTED) {
    // FLOAT-FREE: index the offline-baked Q16.16 V*P table by integer frame.
    // The committed bytes are identical host and device -> bit-identical
    // fb_crc. g_scripted_mvp[i] is fx16_16[16] == struct Mat4fx layout.
    uint32_t const idx = cam->frame % (uint32_t)SCRIPTED_FRAME_COUNT;
    vp = (const struct Mat4fx*)g_scripted_mvp[idx];
  } else {
    // FREE-FLY (interactive, non-profiled): bake from the integer eye/look.
    // Float is allowed ONLY here; this path is never fb_crc-compared.
    double const ex = (double)cam->eye[0] / 65536.0;
    double const ey = (double)cam->eye[1] / 65536.0;
    double const ez = (double)cam->eye[2] / 65536.0;
    double const lx = (double)cam->look[0] / 65536.0;
    double const ly = (double)cam->look[1] / 65536.0;
    double const lz = (double)cam->look[2] / 65536.0;
    // 1:1 aspect, fill 240x240. near/far cover the pre-scaled field. (Must
    // match gen_camera_path.py's FOV/aspect/near/far so the two paths agree.)
    m_perspective(&s_terrain_proj, 50.0, 1.0, 0.5, 80.0);
    m_lookat(&s_terrain_view, ex, ey, ez, lx, ly, lz, 0.0, 1.0, 0.0);
    mat4_mul(&s_freefly_vp, &s_terrain_proj, &s_terrain_view);
    vp = &s_freefly_vp;
  }

  struct Command c;
  memset(&c, 0, sizeof c);
  c.u.clear.color = rgb565(16, 24, 48);  // panorama-ish dark blue
  push(CMD_CLEAR, &c);

  memset(&c, 0, sizeof c);
  c.u.set_matrix.target = MTX_PROJECTION;
  c.u.set_matrix.push = 0;
  c.u.set_matrix.mat = vp;
  push(CMD_SET_MATRIX, &c);

  memset(&s_terrain_state, 0, sizeof s_terrain_state);
  s_terrain_state.zmode = ZMODE_OPAQUE;
  s_terrain_state.cull = CULL_BACK;
  s_terrain_state.lit = 0;  // flat-shaded pre-lit debug colors
  memset(&c, 0, sizeof c);
  c.u.set_material.state = &s_terrain_state;
  push(CMD_SET_MATERIAL, &c);

  memset(&c, 0, sizeof c);
  c.u.set_matrix.target = MTX_MODELVIEW;
  c.u.set_matrix.push = 1;
  c.u.set_matrix.mat = &s_terrain_model;
  push(CMD_SET_MATRIX, &c);

  // ---- terrain mesh: single LOAD_VERTS + DRAW_TRIS batch ----
  // LOAD_VERTS.count is driven by the count RETURNED from demo_terrain_geometry
  // (s_terrain_vcount), not the DEMO_TERRAIN_VERTS macro, so the T0 swap to
  // D.1's real mesh can't silently mismatch the loaded window.
  memset(&c, 0, sizeof c);
  c.u.load_verts.ptr = s_terrain_vtx;
  c.u.load_verts.count = (uint16_t)s_terrain_vcount;
  push(CMD_LOAD_VERTS, &c);
  telem->cmdgen_verts += s_terrain_vcount;
  memset(&c, 0, sizeof c);
  c.u.draw_tris.idx = s_terrain_idx;
  c.u.draw_tris.tri_count = (uint16_t)DEMO_TERRAIN_TRIS;
  push(CMD_DRAW_TRIS, &c);
  telem->cmdgen_draws += 1;
  telem->cmdgen_tris += DEMO_TERRAIN_TRIS;

  // ---- tree billboards: second batch (same model transform) ----
  memset(&c, 0, sizeof c);
  c.u.load_verts.ptr = s_tree_vtx;
  c.u.load_verts.count = (uint16_t)s_tree_vcount;
  push(CMD_LOAD_VERTS, &c);
  telem->cmdgen_verts += s_tree_vcount;
  memset(&c, 0, sizeof c);
  c.u.draw_tris.idx = s_tree_idx;
  c.u.draw_tris.tri_count = (uint16_t)DEMO_TREE_TRIS;
  push(CMD_DRAW_TRIS, &c);
  telem->cmdgen_draws += 1;
  telem->cmdgen_tris += DEMO_TREE_TRIS;

  push(CMD_POP_MATRIX, &c);

  memset(&c, 0, sizeof c);
  push(CMD_END, &c);  // streams MUST be END-terminated

  telem->cmdgen_commands = s_cb.count;
  return s_cb.count;
}
