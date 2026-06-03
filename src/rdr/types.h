// rdr/types.h — FROZEN renderer contract (Stream A). POD only; C headers; no
// behavior. All cross-module data structures live here; module headers declare
// only their own module_verb() functions and include this. Orthodox C++ (see
// CLAUDE.md).
#ifndef RDR_TYPES_H
#define RDR_TYPES_H

#include <stdint.h>

// ---- error codes (0 = ok, errno-like otherwise) ----------------------------
enum RdrErr {
  RDR_OK = 0,
  RDR_ENOMEM = 1,      // arena/bin exhausted
  RDR_EOVERFLOW = 2,   // cap exceeded (triangles/verts dropped, counted)
  RDR_EINVAL = 3,      // bad argument
  RDR_EDEGENERATE = 4  // degenerate triangle rejected
};

// ---- fixed-point formats (see skills/fixed-point) --------------------------
typedef int32_t fx16_16;  // matrices / transform (Q16.16)
typedef int32_t fx12_4;   // screen coords (Q12.4, subpixel edge eval)
typedef int32_t fx_invw;  // 1/w depth (w-buffer)

struct Vec3fx {
  fx16_16 x, y, z;
};
struct Mat4fx {
  fx16_16 m[16];
};  // column-major

// ---- input vertex (GBI Vtx-union style, 16 B) ------------------------------
// Per-DRAW_TRIS/material flag selects the active union arm (pre-lit vs lit).
struct Vtx {
  int16_t pos[3];  // model-space position
  int16_t uv[2];   // S10.5 texcoords
  union {
    uint8_t rgba[4];  // pre-lit path
    struct {
      int8_t n[3];
      uint8_t a;
    } nrm;  // normal + alpha (lit path)
  } c;
};

// ---- compact transformed vertex (~14 B; retained in the sort-middle pool) --
struct TVtx {
  fx12_4 x, y;    // screen position (Q12.4)
  fx_invw inv_w;  // 1/w
  int16_t u_iw;   // u * inv_w
  int16_t v_iw;   // v * inv_w
  uint16_t rgba;  // packed shade color (565/4444 per config)
  uint8_t fog;    // D1 (terrain wave): per-vertex fog factor [0,255], 0=none.
                  // geom births it 0; R.3-fog populates (geom_fog_factor); clip
                  // lerps it; raster interps+applies. Fills the former implicit
                  // tail pad -> TVtx size unchanged (20 B). Read-only until
                  // R.3-fog, but always defined where born (no stray bytes).
};

// ---- tile bin: per-tile triangle refs (indices into the TVtx pool) ---------
struct TriRef {
  uint16_t v0, v1, v2;  // indices into the transformed-vertex pool
  uint16_t material;    // material id (render-state version)
};
struct TileBin {
  struct TriRef* refs;  // arena-allocated segment
  uint32_t count;       // used
  uint32_t cap;         // capacity (overflow drops + counts, never corrupts)
  uint32_t dropped;     // dropped-on-overflow counter (debug-surfaced)
};

// ---- render-state block (mutated by SET_* commands; versioned/interned) ----
enum ZMode { ZMODE_OPAQUE = 0, ZMODE_XLU = 1, ZMODE_DECAL = 2 };
enum CullMode { CULL_BACK = 0, CULL_FRONT = 1, CULL_NONE = 2, CULL_BOTH = 3 };

struct TexDesc {
  const void* data;  // texels in flash/XIP
  uint16_t w, h;
  uint8_t format;  // RGBA565/4444, IA8/IA4, I8/I4, CI4/CI8 (see TexFormat)
  uint8_t wrap_s, wrap_t;  // WRAP/MIRROR/CLAMP
  uint8_t filter;          // POINT / THREE_POINT
  uint8_t mip_levels;
  const void* tlut;  // CI palette (SRAM-resident), else null
};
enum TexFormat {
  TEXFMT_RGBA565 = 0,
  TEXFMT_RGBA4444,
  TEXFMT_IA8,
  TEXFMT_IA4,
  TEXFMT_I8,
  TEXFMT_I4,
  TEXFMT_CI4,
  TEXFMT_CI8,
  // Δ3 (terrain wave): N64-native RGBA16 = 5-5-5-1. Appended (preserves prior
  // values) — faithful tree sprites (1-bit TEX_EDGE cutout alpha) + panorama
  // TLUT entries. The single contract delta the terrain port needs at this
  // barrier; additive, no struct-layout change.
  TEXFMT_RGBA5551
};
enum WrapMode { WRAP_REPEAT = 0, WRAP_MIRROR = 1, WRAP_CLAMP = 2 };
enum TexFilter { FILTER_POINT = 0, FILTER_THREE_POINT = 1 };

struct CombinerState {
  uint8_t mode;
  uint8_t a, b, c, d;
};  // (A-B)*C+D, inputs are enum ids
struct FogState {
  fx16_16 near_z, far_z;
  uint16_t color;
  uint8_t enabled;
};
struct Light {
  int8_t dir[3];
  uint8_t pad;
  uint8_t rgb[3];
  uint8_t pad2;
};  // post-modelview dir
struct LightState {
  struct Light dirs[8];
  uint8_t count;
  uint8_t ambient[3];
};

struct RenderState {
  struct TexDesc tex;
  struct CombinerState combiner;
  struct FogState fog;
  struct LightState lights;
  uint16_t prim_color, env_color;
  uint8_t zmode;      // enum ZMode
  uint8_t cull;       // enum CullMode
  uint8_t alpha_cmp;  // 0 = off, else threshold
  uint8_t texgen;     // 0=off, 1=hilite, 2=env
  uint8_t lit;        // 0 = pre-lit RGBA, 1 = normal+lighting
};

// ---- command stream (tagged union) -----------------------------------------
enum CmdOp {
  CMD_SET_MATRIX = 0,
  CMD_POP_MATRIX,
  CMD_SET_VIEWPORT,
  CMD_SET_MATERIAL,
  CMD_SET_COMBINER,
  CMD_SET_PRIM_COLOR,
  CMD_SET_ENV_COLOR,
  CMD_SET_FOG,
  CMD_SET_RENDERMODE,
  CMD_SET_LIGHTS,
  CMD_SET_TEXGEN,
  CMD_LOAD_VERTS,
  CMD_DRAW_TRIS,
  CMD_CULL_VOLUME,
  CMD_BRANCH_LESS_Z,
  CMD_CALL_LIST,
  CMD_CLEAR,
  CMD_RETURN,
  CMD_END
};
enum MatrixTarget { MTX_MODELVIEW = 0, MTX_PROJECTION = 1 };

struct Command {
  uint8_t op;  // enum CmdOp
  union {
    struct {
      uint8_t target;
      uint8_t push;
      const struct Mat4fx* mat;
    } set_matrix;
    struct {
      int16_t x, y, w, h;
    } viewport;
    struct {
      const struct RenderState* state;
    } set_material;
    struct {
      uint16_t color;
    } set_color;  // prim/env
    struct {
      fx16_16 near_z, far_z;
      uint16_t color;
    } set_fog;
    struct {
      const struct Vtx* ptr;
      uint16_t count;
      uint16_t base;
    } load_verts;
    struct {
      const uint16_t* idx;
      uint16_t tri_count;
    } draw_tris;
    struct {
      const struct Vtx* hull;
      uint16_t count;
    } cull_volume;
    struct {
      uint16_t vtx;
      fx_invw z;
      const struct Command* dl;
    } branch_less_z;
    struct {
      const struct Command* ptr;
    } call_list;
    struct {
      uint16_t color;
    } clear;
  } u;
};

// ---- frame handle (opaque-ish; arenas + pools the pipeline fills) -----------
// Field layout is intentionally minimal here; modules extend behavior, not
// layout.
struct Frame;  // forward-declared; defined where the renderer owns it (rdr.cc)

#endif  // RDR_TYPES_H
