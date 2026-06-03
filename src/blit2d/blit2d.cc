// blit2d.cc — implementation (Wave-D stream D.5: blit2d). Orthodox C++.
//
// Screen-space 2D sky background blits. No depth, no zbuf; opaque RGB565
// (gfx/framebuffer.h color_t / rgb565()). All per-pixel addressing is integer:
// horizontal scroll wraps mod src_w; the vertical band is an integer rescale of
// the source rows plus an elevation offset (camera pitch). Floats appear only
// in the setup helper blit2d_horizon_row_1to1 (called once, never per pixel).
//
// Sampling (approach (b), self-contained): tex.h's tex_sample() on main is
// RGBA565/4444 only (no CI/I), so this module decodes CI8+RGBA5551-TLUT and I8
// locally and POINT-samples. TODO(T3): swap blit2d_decode_ci8/_i8 for
// tex_sample once it grows CI8/I8 (point filter); see blit2d.h.
#include "blit2d/blit2d.h"

#include <stddef.h>

#include "rdr/config.h"

// ---- RGBA5551 (N64-native) -> RGB565 ---------------------------------------
// 5551: R[15:11] G[10:6] B[5:1] A[0]. 565 wants 5/6/5; expand green 5->6 by
// bit-replication (g6 = g5<<1 | g5>>4). The alpha bit is dropped (565 opaque).
static uint16_t b2d_5551_to_565(uint16_t px) {
  int r5 = (px >> 11) & 0x1F;
  int g5 = (px >> 6) & 0x1F;
  int b5 = (px >> 1) & 0x1F;
  int g6 = (g5 << 1) | (g5 >> 4);
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// ---- RGB565 channel pack/unpack helpers ------------------------------------
static int b2d_r5(uint16_t c) { return (c >> 11) & 0x1F; }
static int b2d_g6(uint16_t c) { return (c >> 5) & 0x3F; }
static int b2d_b5(uint16_t c) { return c & 0x1F; }
static uint16_t b2d_pack565(int r5, int g6, int b5) {
  return (uint16_t)((r5 << 11) | (g6 << 5) | b5);
}

// ---- 8-bit alpha-over with rounding (src `cloud` over `grad`) --------------
static int b2d_over8(int a, int cloud, int grad) {
  return (a * cloud + (255 - a) * grad + 127) / 255;
}

// ============================================================================
// Public leaf kernels
// ============================================================================

int blit2d_horizon_row_1to1(int src_horizon_row, int src_h, int dst_h) {
  if (src_h <= 0) return 0;
  // Rounded vertical rescale: dst = round(src_row * dst_h / src_h).
  int row = (src_horizon_row * dst_h + src_h / 2) / src_h;
  if (row < 0) row = 0;
  if (dst_h > 0 && row > dst_h - 1) row = dst_h - 1;
  return row;
}

uint16_t blit2d_decode_ci8(const void* ci8, const void* tlut, int w, int x,
                           int y) {
  if (ci8 == 0 || tlut == 0) return 0;
  const uint8_t* idx = (const uint8_t*)ci8;
  const uint16_t* pal = (const uint16_t*)tlut;
  uint8_t i = idx[(size_t)y * (size_t)w + (size_t)x];  // POINT sample
  return b2d_5551_to_565(pal[i]);
}

uint8_t blit2d_decode_i8(const void* i8, int w, int x, int y) {
  if (i8 == 0) return 0;
  const uint8_t* p = (const uint8_t*)i8;
  return p[(size_t)y * (size_t)w + (size_t)x];
}

// ============================================================================
// blit2d_panorama — scrolling CI8 cylinder
// ============================================================================

RdrErr blit2d_panorama(const struct Blit2dRect* r, uint16_t* fb) {
  if (r == 0 || fb == 0) return RDR_EINVAL;
  if (r->mode != BLIT2D_PANORAMA) return RDR_EINVAL;
  if (r->src == 0 || r->tlut == 0) return RDR_EINVAL;
  if (r->src_w == 0 || r->src_h == 0 || r->dst_w == 0 || r->dst_h == 0)
    return RDR_EINVAL;

  int srcw = (int)r->src_w;
  int srch = (int)r->src_h;
  int dstw = (int)r->dst_w;
  int dsth = (int)r->dst_h;

  // dst rect clamped to the framebuffer.
  int x0 = (int)r->dst_x;
  int y0 = (int)r->dst_y;

  for (int dy = 0; dy < dsth; ++dy) {
    int py = y0 + dy;
    if (py < 0 || py >= RDR_SCREEN_H) continue;
    // Vertical band: integer rescale of source rows + elevation (camera pitch),
    // clamped to [0, srch-1] (no vertical wrap — the cylinder is a band).
    int sy = (dy * srch) / dsth + (int)r->elevation;
    if (sy < 0) sy = 0;
    if (sy >= srch) sy = srch - 1;
    uint16_t* row = fb + (size_t)py * RDR_SCREEN_W;
    for (int dx = 0; dx < dstw; ++dx) {
      int px = x0 + dx;
      if (px < 0 || px >= RDR_SCREEN_W) continue;
      // Horizontal wrap: source column = (scroll_x + dx) mod src_w. Integer
      // stepping; mod keeps the cylinder seamless across src_w.
      int sx = ((int)r->scroll_x + dx) % srcw;
      row[px] = blit2d_decode_ci8(r->src, r->tlut, srcw, sx, sy);
    }
  }
  return RDR_OK;
}

// ============================================================================
// blit2d_clouds — I8 alpha over a procedural blue gradient
// ============================================================================

RdrErr blit2d_clouds(const struct Blit2dRect* r, uint16_t* fb) {
  if (r == 0 || fb == 0) return RDR_EINVAL;
  if (r->mode != BLIT2D_CLOUDS) return RDR_EINVAL;
  if (r->src == 0) return RDR_EINVAL;
  if (r->src_w == 0 || r->src_h == 0 || r->dst_w == 0 || r->dst_h == 0)
    return RDR_EINVAL;

  int srcw = (int)r->src_w;
  int srch = (int)r->src_h;
  int dstw = (int)r->dst_w;
  int dsth = (int)r->dst_h;
  int x0 = (int)r->dst_x;
  int y0 = (int)r->dst_y;

  // Gradient endpoints, unpacked once.
  int tr = b2d_r5(r->sky_top), tg = b2d_g6(r->sky_top), tb = b2d_b5(r->sky_top);
  int hr = b2d_r5(r->sky_horizon), hg = b2d_g6(r->sky_horizon),
      hb = b2d_b5(r->sky_horizon);
  int cr = b2d_r5(r->cloud_color), cg = b2d_g6(r->cloud_color),
      cb = b2d_b5(r->cloud_color);

  // Gradient spans [dst_y, horizon_row]; below the horizon clamps to sky_horizon.
  int span = (int)r->horizon_row - y0;

  for (int dy = 0; dy < dsth; ++dy) {
    int py = y0 + dy;
    if (py < 0 || py >= RDR_SCREEN_H) continue;

    // Procedural vertical gradient (integer lerp top -> horizon by row).
    int gr, gg, gb;
    if (span <= 0) {
      gr = hr;
      gg = hg;
      gb = hb;
    } else {
      int t = py - y0;
      if (t < 0) t = 0;
      if (t > span) t = span;
      gr = tr + (hr - tr) * t / span;
      gg = tg + (hg - tg) * t / span;
      gb = tb + (hb - tb) * t / span;
    }

    int sy = (dy * srch) / dsth;
    if (sy >= srch) sy = srch - 1;

    uint16_t* row = fb + (size_t)py * RDR_SCREEN_W;
    for (int dx = 0; dx < dstw; ++dx) {
      int px = x0 + dx;
      if (px < 0 || px >= RDR_SCREEN_W) continue;
      int sx = (dx * srcw) / dstw;
      if (sx >= srcw) sx = srcw - 1;
      int a = (int)blit2d_decode_i8(r->src, srcw, sx, sy);  // I8 intensity = α
      int rr = b2d_over8(a, cr, gr);
      int rg = b2d_over8(a, cg, gg);
      int rb = b2d_over8(a, cb, gb);
      row[px] = b2d_pack565(rr, rg, rb);
    }
  }
  return RDR_OK;
}

// ============================================================================
// blit2d_render — dispatch
// ============================================================================

RdrErr blit2d_render(const struct Blit2dRect* r, uint16_t* fb) {
  if (r == 0) return RDR_EINVAL;
  switch (r->mode) {
    case BLIT2D_PANORAMA:
      return blit2d_panorama(r, fb);
    case BLIT2D_CLOUDS:
      return blit2d_clouds(r, fb);
    default:
      return RDR_EINVAL;
  }
}
