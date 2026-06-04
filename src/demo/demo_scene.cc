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
#include "demo/assets/terrain/terrain_atlas.h"  // T1 gutter'd terrain atlas (512^2 5551)
#include "demo/assets/terrain/terrain_grid_idx.h"  // shared per-tile index pattern
#include "demo/assets/terrain/terrain_grid_vtx.h"  // D.1 baked terrain mesh (pos/UV)
#include "demo/assets/terrain/terrain_panorama.h"  // T3a CI8 512x64 sky panorama
#include "demo/assets/terrain/terrain_tlut.h"      // T3a panorama RGBA5551 TLUT
#include "demo/assets/terrain/terrain_tree0.h"  // T1 tree0 billboard sprite (32x64 5551)
#include "demo/camera_path_gen.h"  // committed Q16.16 scripted V*P + sky table
#include "demo/debug_terrain_gen.h"  // committed FLASH-CONST debug-colored geometry
#include "fixed/fixed.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"  // enum Button (free-fly toggle/nudge)
#include "rdr/frame.h"  // struct Frame (blits[]/blit_count), RDR_SCREEN_*
#include "shade/shade.h"  // CC_* combiner mux ids + COMBINE_* presets (T1 materials)

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
// D.1's REAL baked mesh is a GROUND height-field in the XZ plane (Y up): the
// camera flies above and looks down/forward, so the field fills the frame (the
// "near-terrain fill" worst case). Positions/UVs come verbatim from the baked
// asset (g_terrain_grid_vtx, pre-scaled int16 model coords; modelview is
// identity, verts authored directly in pre-scaled world). demo_terrain_geometry
// overrides ONLY the per-vertex color (debug palette) and expands the shared
// per-tile index pattern; the rest of the build is geometry-agnostic.
#define TERRAIN_BASE_Y 0 /* ground plane Y (tree quads stand on it) */

// Distinct debug color per mesh-cell (N64 texture-window tile). A hashed
// palette spreads adjacent cells across hue so a one-blob / wrong-winding
// render is obvious. (NOT the N64 flat gray — that would blind the orientation
// check, per the D.7 brief.) All 9 verts of a tile share its color, so each
// tile renders as one flat patch; tile boundaries + overall shape pin
// transform/cull/winding.
static void tile_debug_color(int tile, uint8_t* rgb) {
  // 3-channel saw waves with coprime strides -> many distinct buckets over 128.
  rgb[0] = (uint8_t)(64 + ((tile * 37) & 0xBF));
  rgb[1] = (uint8_t)(64 + (((tile * 53) + 17) & 0xBF));
  rgb[2] = (uint8_t)(64 + (((tile * 61) + 31) & 0xBF));
}

uint32_t demo_terrain_geometry(struct Vtx* verts, uint16_t* idx) {
  // Asset stride is the packed 14-byte Vtx (pos[3]+uv[2]+rgba[4]); copy those
  // field-bytes verbatim (alignment-safe; robust to any struct tail pad), then
  // override the gray rgba with the per-mesh-cell debug color (tile-major:
  // mesh-cell = vert/9).
  size_t const stride =
      (size_t)g_terrain_grid_vtx_len / (unsigned)DEMO_TERRAIN_VERTS;
  for (int i = 0; i < DEMO_TERRAIN_VERTS; ++i) {
    memcpy(&verts[i], g_terrain_grid_vtx + ((size_t)i * stride), stride);
    uint8_t rgb[3];
    tile_debug_color(i / DEMO_TERRAIN_VERTS_PER_TILE, rgb);
    verts[i].c.rgba[0] = rgb[0];
    verts[i].c.rgba[1] = rgb[1];
    verts[i].c.rgba[2] = rgb[2];
    verts[i].c.rgba[3] = 255;
  }
  // Expand the shared per-tile index pattern over all tiles. Tile t's 9 verts
  // occupy [t*9, t*9+9), so its triangle indices are pattern[k] + t*9. The
  // pattern is already wound up-facing for our CULL_BACK (the converter's
  // bake_terrain_tile_indices adapts the N64 winding — T0 finding), so expand
  // verbatim.
  int o = 0;
  for (int t = 0; t < DEMO_TERRAIN_TILES; ++t) {
    int const vbase = t * DEMO_TERRAIN_VERTS_PER_TILE;
    for (unsigned k = 0; k < g_terrain_tile_indices_len; ++k) {
      idx[o++] = (uint16_t)(g_terrain_tile_indices[k] + vbase);
    }
  }
  return (uint32_t)DEMO_TERRAIN_VERTS;
}

uint32_t demo_tree_geometry(struct Vtx* verts, uint16_t* idx) {
  // Tree billboard quads: upright planes STANDING on the ground (extending in
  // +Y), scattered over the field at distinct positions, textured with the
  // tree0 sprite (MODULATE = TEXEL x SHADE) under a gray Gouraud gradient. The
  // quad spans X (width) x Y (height); double-sided (CULL_NONE, N6). Two tris
  // per quad. NOT view-aligned (true billboarding needs per-frame float; a
  // static upright quad keeps the per-frame path float-free) — fine for T1.
  //
  // UVs map the 32x64 tree0 sprite upright (S10.5 = texel*32 -> u in {0,1024},
  // v in {0,2048}; texture v=0 at top). Per-corner: 0 bl=(0,2048) 1 br=(1024,
  // 2048) 2 tl=(0,0) 3 tr=(1024,0). The per-vertex color is the N64 tree's gray
  // gradient: top corners (2,3) = 206, bottom corners (0,1) = 157 — exercises
  // the Gouraud interp path through the MODULATE combiner.
  int o = 0;
  for (int t = 0; t < DEMO_TREE_COUNT; ++t) {
    // Deterministic scatter inside the field (XZ ground positions).
    int const cx = -12 + ((t * 7) % 25);
    int const cz = -12 + ((t * 11) % 25);
    int const hw = 2;  // billboard half-width (world units, along X)
    int const ht = 5;  // billboard height (world units, along +Y)
    int const base = t * 4;
    // Corners: 0 bl, 1 br, 2 tl, 3 tr (Y up, X across; Z fixed at cz).
    int const corner[4][2] = {{-hw, 0}, {hw, 0}, {-hw, ht}, {hw, ht}};
    // Per-corner sprite UVs (S10.5) and gray Gouraud gradient (top 206 / bot
    // 157), matched to the corner order above.
    int const corner_uv[4][2] = {{0, 2048}, {1024, 2048}, {0, 0}, {1024, 0}};
    uint8_t const corner_gray[4] = {157, 157, 206, 206};
    for (int k = 0; k < 4; ++k) {
      struct Vtx* v = &verts[base + k];
      memset(v, 0, sizeof *v);
      v->pos[0] = (int16_t)(cx + corner[k][0]);
      v->pos[1] = (int16_t)(TERRAIN_BASE_Y + corner[k][1]);
      v->pos[2] = (int16_t)cz;
      v->uv[0] = (int16_t)corner_uv[k][0];
      v->uv[1] = (int16_t)corner_uv[k][1];
      uint8_t const gray = corner_gray[k];
      v->c.rgba[0] = gray;
      v->c.rgba[1] = gray;
      v->c.rgba[2] = gray;
      v->c.rgba[3] = 255;
    }
    uint16_t const a = (uint16_t)(base + 0);
    uint16_t const b = (uint16_t)(base + 1);
    uint16_t const c2 = (uint16_t)(base + 2);
    uint16_t const d = (uint16_t)(base + 3);
    // Two tris per quad (a,c,b / b,c,d). The tree material is CULL_NONE (N6
    // double-sided billboard), so the quad is visible from either hemisphere —
    // the winding sense no longer culls a side. Order kept verbatim from the
    // debug placeholder so the geometry is unchanged but for uv + color.
    idx[o++] = a;
    idx[o++] = c2;
    idx[o++] = b;
    idx[o++] = b;
    idx[o++] = c2;
    idx[o++] = d;
  }
  return (uint32_t)(DEMO_TREE_COUNT * 4);
}

// ---- module-local matrix / state storage (referenced by the built stream) --
// The INPUT geometry (terrain + tree verts/indices) is NOT here: it lives in
// FLASH as committed const arrays (debug_terrain_gen.h, g_debug_*), so it stays
// out of .bss (RAM). The integration gate hit an RP2040 RAM overflow because
// the placeholder geometry was computed into ~13 KB of static buffers at init;
// baking it to flash-const frees that RAM. The demo submits LOAD_VERTS/
// DRAW_TRIS pointers straight at the const arrays. The runtime
// demo_terrain_geometry/demo_tree_geometry FILL functions remain (host tests +
// the T0 D.1 swap seam use the fill API), but the device demo never fills RAM.
static struct Mat4fx s_terrain_proj;
static struct Mat4fx s_terrain_view;
static struct Mat4fx s_freefly_vp;     // free-fly only: projection * view
static struct Mat4fx s_terrain_model;  // identity (verts authored in world)
static struct RenderState s_terrain_state;
static struct RenderState s_tree_state;  // opaque cutout pass

// ---- T1 material fillers (single source of truth) --------------------------
// The build below populates its statics through THESE, and tests assert the
// bound material through THESE, so the constants live in exactly one place.
void demo_terrain_material(struct RenderState* out) {
  memset(out, 0, sizeof *out);
  out->tex.data = g_terrain_atlas;
  out->tex.w = (uint16_t)g_terrain_atlas_w;  // 512
  out->tex.h = (uint16_t)g_terrain_atlas_h;  // 512
  out->tex.format = (uint8_t)TEXFMT_RGBA5551;
  // pow2 512 atlas: REPEAT mask-wrap is the S0-locked sampler discipline. UVs
  // are absolute within [0,512) so it samples in-range.
  out->tex.wrap_s = (uint8_t)WRAP_REPEAT;
  out->tex.wrap_t = (uint8_t)WRAP_REPEAT;
  // T2: bilinear (FILTER_THREE_POINT). The atlas is a gutter'd 512^2 pow2 build
  // (design P3-1) — each tile's sub-region is bordered by a replicated gutter
  // so a bilinear tap reads the tile's own replicated edge, NOT its neighbor.
  // The baked absolute UVs already address those gutter'd sub-regions, so this
  // is seam-correct. Bilinear is a SAMPLE-time op: it does not change u_iw
  // birth, so the T1 Δ4 int16 bound still holds (no geom change).
  out->tex.filter = (uint8_t)FILTER_THREE_POINT;
  out->tex.mip_levels = 1;
  out->tex.tlut = 0;
  // P4-2 ENV wiring: TEXEL0 x ENV. Without it the terrain would be untinted.
  out->combiner.mode = (uint8_t)COMBINE_CUSTOM;
  out->combiner.a = (uint8_t)CC_TEXEL0;
  out->combiner.b = (uint8_t)CC_ZERO;
  out->combiner.c = (uint8_t)CC_ENVIRONMENT;
  out->combiner.d = (uint8_t)CC_ZERO;
  // T1 placeholder ENV tint; faithful N64 ENV resolved at T4 from the RDP
  // combine. Light sage: deterministic, provably non-identity, preserves the
  // atlas hue variety.
  out->env_color = rgb565(216, 232, 200);
  out->zmode = (uint8_t)ZMODE_OPAQUE;
  out->cull = (uint8_t)CULL_BACK;
  out->alpha_cmp = 0;
  out->lit = 0;
}

void demo_tree_material(struct RenderState* out) {
  memset(out, 0, sizeof *out);
  out->tex.data = g_terrain_tree0;
  out->tex.w = 32;
  out->tex.h = 64;
  out->tex.format = (uint8_t)TEXFMT_RGBA5551;
  // CLAMP so the sprite edge does not wrap.
  out->tex.wrap_s = (uint8_t)WRAP_CLAMP;
  out->tex.wrap_t = (uint8_t)WRAP_CLAMP;
  // Trees stay POINT (crisp cutout edge); do NOT bilinear the trees.
  out->tex.filter = (uint8_t)FILTER_POINT;
  out->tex.mip_levels = 1;
  out->tex.tlut = 0;
  out->combiner.mode = (uint8_t)COMBINE_MODULATE;  // TEXEL x SHADE
  out->zmode = (uint8_t)ZMODE_OPAQUE;
  out->cull = (uint8_t)CULL_NONE;  // N6 double-sided billboard
  // T2 opaque cutout pass (N64 G_RM_AA_ZB_TEX_EDGE, combiner alpha = TEXEL0).
  // The tree0 sprite is 5551 (1-bit alpha) -> decoded texel alpha is 0 or 255,
  // so ANY threshold in (0,255] cuts the same silhouette; 128 is the canonical
  // midpoint. This activates raster's alpha_cmp discard path, killing the black
  // billboard background and leaving only the tree silhouette (the visible T2
  // win).
  out->alpha_cmp = 128;
  out->lit = 0;
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

// ---- T3a panorama sky blit -------------------------------------------------
// The committed g_terrain_panorama is a 512x64 CI8 cylinder (DEMO_PANORAMA_W is
// the 360-deg circumference, defined in camera_path_gen.h). It is a FULL
// surround scene (sky / hills / water reflection), not a sky gradient, so it is
// drawn as a FULL-FRAME cylindrical BACKDROP: the source wraps horizontally by
// the view azimuth (scroll_x) and stretches over the whole frame. The 3D sweep
// rasterizes over it (z-write, no zbuf seed), so the seam where sky meets the
// terrain is just the terrain silhouette — inherently gap-free (T3 N4). The
// per-frame projected horizon_row is NOT needed to place this backdrop, but is
// carried in the descriptor (panorama ignores it) for T3b clouds. (A thin
// horizon band was prototyped but the surround art reads better full-frame.)
#define DEMO_PANORAMA_H 64

// Free-fly sky (sanctioned interactive-float exception; never fb_crc-compared):
// recompute scroll_x (view azimuth) + horizon_row (level ray; here a no-roll
// small-angle form ndc_y = -fscale*fy/hlen, geom.cc viewport map row =
// H/2*(1-ndc_y)) from the integer eye/look in float. APPROXIMATES (not bit-for-
// bit) gen_camera_path.sky_for_frame's full V*P projection — they agree
// VISUALLY; exactness is unneeded since free-fly is never fb_crc-compared.
static void freefly_sky(const struct DemoCamera* cam, int* scroll_x,
                        int* horizon_row) {
  double const pi = 3.14159265358979323846;
  double const ex = (double)cam->eye[0] / 65536.0;
  double const ey = (double)cam->eye[1] / 65536.0;
  double const ez = (double)cam->eye[2] / 65536.0;
  double const fx = ((double)cam->look[0] / 65536.0) - ex;
  double const fy = ((double)cam->look[1] / 65536.0) - ey;
  double const fz = ((double)cam->look[2] / 65536.0) - ez;
  int sx = (int)lrint(atan2(fx, fz) / (2.0 * pi) * (double)DEMO_PANORAMA_W);
  sx %= DEMO_PANORAMA_W;
  if (sx < 0) {
    sx += DEMO_PANORAMA_W;
  }
  *scroll_x = sx;
  double const hlen = sqrt((fx * fx) + (fz * fz));
  if (hlen <= 0.0) {
    *horizon_row = RDR_SCREEN_H / 2;
    return;
  }
  double const fscale = 1.0 / tan(50.0 * 0.5 * pi / 180.0);
  double const ndc_y = -fscale * fy / hlen;
  int row = (int)lrint((RDR_SCREEN_H * 0.5) * (1.0 - ndc_y));
  if (row < 0) {
    row = 0;
  }
  if (row > RDR_SCREEN_H - 1) {
    row = RDR_SCREEN_H - 1;
  }
  *horizon_row = row;
}

void demo_fill_blits(struct Frame* f, const struct DemoCamera* cam) {
  int scroll_x;
  int horizon_row;
  if (cam->mode == DEMO_CAM_SCRIPTED) {
    // FLOAT-FREE: index the committed sky table (host==device fb_crc).
    uint32_t const idx = cam->frame % (uint32_t)SCRIPTED_FRAME_COUNT;
    scroll_x = (int)g_scripted_sky[idx][0];
    horizon_row = (int)g_scripted_sky[idx][1];
  } else {
    freefly_sky(cam, &scroll_x, &horizon_row);
  }
  struct Blit2dRect* b = &f->blits[0];
  memset(b, 0, sizeof *b);
  b->mode = (uint8_t)BLIT2D_PANORAMA;
  b->src = g_terrain_panorama;
  b->tlut = g_terrain_panorama_tlut;
  b->src_w = (uint16_t)DEMO_PANORAMA_W;
  b->src_h = (uint16_t)DEMO_PANORAMA_H;
  b->dst_x = 0;
  b->dst_y = 0;
  b->dst_w = (uint16_t)f->width;
  b->dst_h = (uint16_t)f->height;  // full-frame cylindrical backdrop
  b->scroll_x = (uint16_t)scroll_x;
  b->elevation = 0;  // vertical parallax (pitch) deferred (T4+)
  // Panorama IGNORES horizon_row; carried for T3b clouds (gradient endpoint).
  b->horizon_row = (int16_t)horizon_row;
  f->blit_count = 1;
}

// ---- terrain scene build ---------------------------------------------------
uint32_t demo_terrain_build(struct Command* buf, uint32_t cap,
                            const struct DemoCamera* cam,
                            const struct DemoScroll* scroll,
                            struct DemoTelemetry* telem) {
  // Vert counts driven by the committed const-array sizes (not a separate
  // macro) so a T0 swap to D.1's baked arrays of a different length can't
  // silently mismatch the LOAD_VERTS window.
  uint32_t const terrain_vcount =
      (uint32_t)(sizeof g_debug_terrain_vtx / sizeof g_debug_terrain_vtx[0]);
  uint32_t const tree_vcount =
      (uint32_t)(sizeof g_debug_tree_vtx / sizeof g_debug_tree_vtx[0]);
  // tri_count likewise derives from the committed index-array size (idx/3), so
  // a T0/D.1 mesh swap of a different length can't silently mismatch DRAW_TRIS.
  uint32_t const terrain_tcount = (uint32_t)(sizeof g_debug_terrain_idx /
                                             sizeof g_debug_terrain_idx[0] / 3);
  uint32_t const tree_tcount =
      (uint32_t)(sizeof g_debug_tree_idx / sizeof g_debug_tree_idx[0] / 3);
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

  // T1: terrain + trees use DISTINCT materials (textured atlas vs tree sprite),
  // so each draw is preceded by its own SET_MATERIAL (was one shared untextured
  // state). The fillers are the single source of truth (also used by tests).
  demo_terrain_material(&s_terrain_state);
  demo_tree_material(&s_tree_state);

  memset(&c, 0, sizeof c);
  c.u.set_material.state = &s_terrain_state;
  push(CMD_SET_MATERIAL, &c);

  memset(&c, 0, sizeof c);
  c.u.set_matrix.target = MTX_MODELVIEW;
  c.u.set_matrix.push = 1;
  c.u.set_matrix.mat = &s_terrain_model;
  push(CMD_SET_MATRIX, &c);

  // ---- terrain mesh: textured atlas (TEXEL0 x ENV) ----
  // Submitted straight from the FLASH-CONST arrays (g_debug_*), so no RAM input
  // buffer. count comes from the const-array size (T0-swap-safe; see above).
  memset(&c, 0, sizeof c);
  c.u.load_verts.ptr = g_debug_terrain_vtx;
  c.u.load_verts.count = (uint16_t)terrain_vcount;
  push(CMD_LOAD_VERTS, &c);
  telem->cmdgen_verts += terrain_vcount;
  memset(&c, 0, sizeof c);
  c.u.draw_tris.idx = g_debug_terrain_idx;
  c.u.draw_tris.tri_count = (uint16_t)terrain_tcount;
  push(CMD_DRAW_TRIS, &c);
  telem->cmdgen_draws += 1;
  telem->cmdgen_tris += terrain_tcount;

  // ---- tree billboards: tree0 sprite (MODULATE), double-sided, cutout ------
  // T2 ships the OPAQUE cutout pass only (G_RM_AA_ZB_TEX_EDGE): alpha_cmp
  // discards the transparent billboard background -> z-write the tree
  // silhouette. The faithful XLU surface pass (G_RM_AA_ZB_XLU_SURF) is DEFERRED
  // to T4: at 1-bit 5551 alpha with no coverage-AA yet it composites visually
  // IDENTICALLY to the cutout, and the double-draw is what overflowed the
  // shared bin-ref pool (RDR_BIN_POOL_REFS) — so its payoff waits for AA.
  memset(&c, 0, sizeof c);
  c.u.set_material.state = &s_tree_state;
  push(CMD_SET_MATERIAL, &c);
  memset(&c, 0, sizeof c);
  c.u.load_verts.ptr = g_debug_tree_vtx;
  c.u.load_verts.count = (uint16_t)tree_vcount;
  push(CMD_LOAD_VERTS, &c);
  telem->cmdgen_verts += tree_vcount;
  memset(&c, 0, sizeof c);
  c.u.draw_tris.idx = g_debug_tree_idx;
  c.u.draw_tris.tri_count = (uint16_t)tree_tcount;
  push(CMD_DRAW_TRIS, &c);
  telem->cmdgen_draws += 1;
  telem->cmdgen_tris += tree_tcount;

  push(CMD_POP_MATRIX, &c);

  memset(&c, 0, sizeof c);
  push(CMD_END, &c);  // streams MUST be END-terminated

  telem->cmdgen_commands = s_cb.count;
  return s_cb.count;
}
