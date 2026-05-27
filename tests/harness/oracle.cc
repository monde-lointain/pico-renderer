// oracle.cc — float reference implementation. See oracle.h.

#include "oracle.h"

#include <math.h>
#include <string.h>

// ---- conversions -----------------------------------------------------------
float omat_from_fx(fx16_16 v) { return (float)v / 65536.0F; }

void oracle_mat_from_fx(struct OMat4* out, const struct Mat4fx* in) {
  for (int i = 0; i < 16; ++i) {
    out->m[i] = omat_from_fx(in->m[i]);
  }
}

fx12_4 oracle_to_q12_4(float screen_px) {
  // Round to nearest 1/16 pixel (Q12.4), matching subpixel edge eval.
  return (fx12_4)lrintf(screen_px * 16.0F);
}

// ---- matrix math (column-major) -------------------------------------------
// m index = col*4 + row.
void oracle_mat_mul_vec(struct OVec4* out, const struct OMat4* m,
                        const struct OVec4* v) {
  const float x = v->x;
  const float y = v->y;
  const float z = v->z;
  const float w = v->w;
  out->x = (m->m[0] * x) + (m->m[4] * y) + (m->m[8] * z) + (m->m[12] * w);
  out->y = (m->m[1] * x) + (m->m[5] * y) + (m->m[9] * z) + (m->m[13] * w);
  out->z = (m->m[2] * x) + (m->m[6] * y) + (m->m[10] * z) + (m->m[14] * w);
  out->w = (m->m[3] * x) + (m->m[7] * y) + (m->m[11] * z) + (m->m[15] * w);
}

void oracle_mat_mul(struct OMat4* out, const struct OMat4* a,
                    const struct OMat4* b) {
  struct OMat4 r;
  for (int col = 0; col < 4; ++col) {
    for (int row = 0; row < 4; ++row) {
      float s = 0.0F;
      for (int k = 0; k < 4; ++k) {
        s += a->m[(k * 4) + row] * b->m[(col * 4) + k];
      }
      r.m[(col * 4) + row] = s;
    }
  }
  *out = r;
}

// ---- transform -------------------------------------------------------------
void oracle_xform_vertex(struct OTVtx* out, const struct OMat4* mvp,
                         const struct OVec4* model_pos, float u, float v,
                         const struct OViewport* vp) {
  struct OVec4 clip;
  oracle_mat_mul_vec(&clip, mvp, model_pos);

  out->u_iw = 0.0F;
  out->v_iw = 0.0F;
  out->clipped = (clip.w <= 0.0F) ? 1 : 0;
  if (out->clipped) {
    out->sx = out->sy = 0.0F;
    out->inv_w = 0.0F;
    return;
  }

  float const inv_w = 1.0F / clip.w;
  // NDC in [-1,1].
  float const ndc_x = clip.x * inv_w;
  float const ndc_y = clip.y * inv_w;

  // GBI viewport map (gbi.h:1173): scale/trans = half the viewport extent.
  // screen = ndc*scale + trans. Y flips (screen-space y grows downward).
  float const half_w = (float)vp->w * 0.5F;
  float const half_h = (float)vp->h * 0.5F;
  float const cx = (float)vp->x + half_w;
  float const cy = (float)vp->y + half_h;
  out->sx = cx + (ndc_x * half_w);
  out->sy = cy - (ndc_y * half_h);
  out->inv_w = inv_w;
  out->u_iw = u * inv_w;
  out->v_iw = v * inv_w;
}

// ---- raster ----------------------------------------------------------------
void oracle_image_clear(struct OImage* img, uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < img->w * img->h; ++i) {
    img->rgb[(i * 3) + 0] = r;
    img->rgb[(i * 3) + 1] = g;
    img->rgb[(i * 3) + 2] = b;
  }
}

// Signed area of edge (relative to point) — orientation test.
// Positive when (px,py) is to the left of directed edge a->b in screen space.
static float edge_fn(float ax, float ay, float bx, float by, float px,
                     float py) {
  return ((bx - ax) * (py - ay)) - ((by - ay) * (px - ax));
}

// Top-left fill rule (RDP / angrylion convention): a sample exactly on an edge
// is covered iff that edge is a "top" or "left" edge of the triangle. For a
// CCW-in-screen-space triangle (we normalize winding below), an edge a->b is:
//   - "left" if it goes downward (by > ay),  [strictly, dy<0 in our y-down? see
//   below]
//   - "top"  if it is horizontal and points left (ay==by and bx<ax).
// We implement it relative to the edge-function sign so winding is handled.
static int is_top_left(float ax, float ay, float bx, float by) {
  // Edge vector a->b. In screen space (y down) for CCW area>0 convention used
  // here, a top edge is horizontal pointing in -x; a left edge goes upward.
  float const ex = bx - ax;
  float const ey = by - ay;
  int const top = (ey == 0.0F && ex < 0.0F);
  int const left = (ey < 0.0F);
  return top || left;
}

void oracle_fill_tri(struct OImage* img, float x0, float y0, float x1, float y1,
                     float x2, float y2, uint8_t r, uint8_t g, uint8_t b) {
  // Normalize to a consistent winding (area > 0) so the fill rule is uniform.
  float const area = edge_fn(x0, y0, x1, y1, x2, y2);
  if (area == 0.0F) {
    return;  // degenerate
  }
  if (area < 0.0F) {
    float const tx = x1;
    float const ty = y1;
    x1 = x2;
    y1 = y2;
    x2 = tx;
    y2 = ty;
  }

  // Bounding box (clamped to image).
  float minxf = x0;
  float maxxf = x0;
  float minyf = y0;
  float maxyf = y0;
  if (x1 < minxf) {
    minxf = x1;
  }
  if (x1 > maxxf) {
    maxxf = x1;
  }
  if (x2 < minxf) {
    minxf = x2;
  }
  if (x2 > maxxf) {
    maxxf = x2;
  }
  if (y1 < minyf) {
    minyf = y1;
  }
  if (y1 > maxyf) {
    maxyf = y1;
  }
  if (y2 < minyf) {
    minyf = y2;
  }
  if (y2 > maxyf) {
    maxyf = y2;
  }

  int minx = (int)floorf(minxf);
  int maxx = (int)ceilf(maxxf);
  int miny = (int)floorf(minyf);
  int maxy = (int)ceilf(maxyf);
  if (minx < 0) {
    minx = 0;
  }
  if (miny < 0) {
    miny = 0;
  }
  if (maxx > img->w) {
    maxx = img->w;
  }
  if (maxy > img->h) {
    maxy = img->h;
  }

  // Per-edge top-left bias: edges are (v0->v1), (v1->v2), (v2->v0).
  int const tl0 = is_top_left(x0, y0, x1, y1);
  int const tl1 = is_top_left(x1, y1, x2, y2);
  int const tl2 = is_top_left(x2, y2, x0, y0);

  for (int py = miny; py < maxy; ++py) {
    for (int px = minx; px < maxx; ++px) {
      float const cxp = (float)px + 0.5F;
      float const cyp = (float)py + 0.5F;
      float const w0 = edge_fn(x1, y1, x2, y2, cxp, cyp);  // opposite v0
      float const w1 = edge_fn(x2, y2, x0, y0, cxp, cyp);  // opposite v1
      float const w2 = edge_fn(x0, y0, x1, y1, cxp, cyp);  // opposite v2
      // Inside if all >0, or ==0 on a top-left edge.
      int const in0 = (w0 > 0.0F) || (w0 == 0.0F && tl1);
      int const in1 = (w1 > 0.0F) || (w1 == 0.0F && tl2);
      int const in2 = (w2 > 0.0F) || (w2 == 0.0F && tl0);
      if (in0 && in1 && in2) {
        size_t const o = (((size_t)py * img->w) + px) * 3;
        img->rgb[o + 0] = r;
        img->rgb[o + 1] = g;
        img->rgb[o + 2] = b;
      }
    }
  }
}

void oracle_unpack565(uint16_t c, uint8_t* r, uint8_t* g, uint8_t* b) {
  // gfx/framebuffer.h rgb565: R in [15:11], G in [10:5], B in [4:0].
  // Expand with bit replication to fill 8 bits (standard 565->888).
  uint8_t const r5 = (uint8_t)((c >> 11) & 0x1F);
  uint8_t const g6 = (uint8_t)((c >> 5) & 0x3F);
  uint8_t const b5 = (uint8_t)(c & 0x1F);
  *r = (uint8_t)((r5 << 3) | (r5 >> 2));
  *g = (uint8_t)((g6 << 2) | (g6 >> 4));
  *b = (uint8_t)((b5 << 3) | (b5 >> 2));
}

// ---- ORACLE_TODO extension stubs ------------------------------------------
// These return nonzero ("unsupported") until the corresponding stream lands its
// float reference. Declared in oracle.h so callers compile against the seam.
int oracle_sample_texel(const struct TexDesc* tex, int s, int t,
                        const uint8_t out_rgba[4]) {
  (void)tex;
  (void)s;
  (void)t;
  (void)out_rgba;
  return 1;  // ORACLE_TODO(tex): per-format decode + wrap + filter
}

int oracle_shade_combiner(const struct CombinerState* cs, const uint8_t* a,
                          const uint8_t* b, const uint8_t* c, const uint8_t* d,
                          const uint8_t out[4]) {
  (void)cs;
  (void)a;
  (void)b;
  (void)c;
  (void)d;
  (void)out;
  return 1;  // ORACLE_TODO(shade): Gouraud interp + (A-B)*C+D
}

int oracle_blend(uint8_t zmode, const uint8_t* src_rgba, float coverage,
                 const uint8_t* dst_rgb) {
  (void)zmode;
  (void)src_rgba;
  (void)coverage;
  (void)dst_rgb;
  return 1;  // ORACLE_TODO(blend): coverage/alpha blend + z compare
}
