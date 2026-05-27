// oracle.h — float reference ("oracle") for the renderer's transform/raster
// math. Slow, readable, floating-point. The fixed-point modules are validated
// against THIS for correctness (not just host<->device parity): it catches
// systematic geometry/fixed-point bugs both targets would otherwise share.
//
// Mirrors documented N64/RDP behavior (see references):
//   - Transform: column-major Mat4fx (Q16.16) -> clip -> perspective divide
//     -> viewport map. Screen output is Q12.4 to match TVtx.x/y.
//   - Viewport (GBI Vp_t): screen = ndc * scale + trans, scale/trans = half
//     the viewport extent (gbi.h:1173). We carry the contract's int16 x,y,w,h.
//   - Raster (flat fill): top-left fill rule (RDP/angrylion convention) — a
//     pixel is covered iff its center is strictly inside, or on a top or left
//     edge.
//
// Currently covered: transform (modelview*projection composed by caller into
// one Mat4fx, or two passed separately) + flat-color triangle fill.
//
// EXTENSION POINTS (filled as modules land — see ORACLE_TODO markers in .cc):
//   - tex: oracle_sample_texel() — per-format decode + wrap + point/3pt filter.
//   - shade: oracle_shade_combiner() — Gouraud interp + (A-B)*C+D combiner.
//   - blend: oracle_blend() — coverage/alpha blend + z compare.
// Each is declared here as a stub the corresponding stream wires up.
//
// Harness carve-out: NOT orthodoxy_enforced; stays C-like regardless.
#ifndef HARNESS_ORACLE_H
#define HARNESS_ORACLE_H

#include <stdint.h>

#include "rdr/types.h"

// ---- math types (float; oracle-internal) ----------------------------------
struct OVec4 {
  float x, y, z, w;
};
struct OMat4 {
  float m[16];
};  // column-major, matches Mat4fx layout

// Viewport: contract's CMD_SET_VIEWPORT payload (int16 x,y,w,h, pixels).
struct OViewport {
  int x, y, w, h;
};

// ---- conversions -----------------------------------------------------------
float omat_from_fx(fx16_16 v);  // Q16.16 -> float
void oracle_mat_from_fx(struct OMat4* out, const struct Mat4fx* in);
// Q12.4 round-trip helper (the oracle emits Q12.4 screen coords like TVtx).
fx12_4 oracle_to_q12_4(float screen_px);

// ---- transform -------------------------------------------------------------
// out = M * v   (M column-major, v as column vector).
void oracle_mat_mul_vec(struct OVec4* out, const struct OMat4* m,
                        const struct OVec4* v);
// Compose: out = a * b (apply b first, then a).
void oracle_mat_mul(struct OMat4* out, const struct OMat4* a,
                    const struct OMat4* b);

// Transformed-vertex oracle output (float superset of TVtx).
struct OTVtx {
  float sx, sy;  // screen x,y in pixels (pre-Q12.4)
  float inv_w;   // 1/w
  float u_iw, v_iw;
  int clipped;  // nonzero if w <= 0 (behind near plane / invalid)
};

// Full vertex transform: clip = MVP * model; perspective divide; viewport map.
// `u`,`v` are raw texcoords carried through as u*inv_w / v*inv_w.
void oracle_xform_vertex(struct OTVtx* out, const struct OMat4* mvp,
                         const struct OVec4* model_pos, float u, float v,
                         const struct OViewport* vp);

// ---- flat-fill raster ------------------------------------------------------
// Render target the oracle draws into (RGB8, row-major, w*h*3 bytes).
struct OImage {
  uint8_t* rgb;
  int w, h;
};
void oracle_image_clear(struct OImage* img, uint8_t r, uint8_t g, uint8_t b);

// Fill a triangle with a single flat RGB color using the top-left fill rule.
// Vertices are screen-space pixel coordinates (float). Backface/winding is NOT
// culled here — the caller decides; the oracle fills any non-degenerate tri.
void oracle_fill_tri(struct OImage* img, float x0, float y0, float x1, float y1,
                     float x2, float y2, uint8_t r, uint8_t g, uint8_t b);

// Unpack an RGB565 color_t into 8-bit RGB (matches gfx/framebuffer.h rgb565()).
void oracle_unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b);

// ---- ORACLE_TODO extension stubs (later streams) --------------------------
// tex: decode one texel to RGBA8 for the given format/coords (point sample).
// Returns 0 ok, nonzero if format/coords unsupported (stub today).
int oracle_sample_texel(const struct TexDesc* tex, int s, int t,
                        uint8_t out_rgba[4]);
// shade: evaluate (A-B)*C+D combiner on 8-bit inputs (stub today).
int oracle_shade_combiner(const struct CombinerState* cs, const uint8_t* a,
                          const uint8_t* b, const uint8_t* c, const uint8_t* d,
                          uint8_t out[4]);
// blend: coverage/alpha blend of src over dst (stub today).
int oracle_blend(uint8_t zmode, const uint8_t* src_rgba, float coverage,
                 uint8_t* dst_rgb);

#endif  // HARNESS_ORACLE_H
