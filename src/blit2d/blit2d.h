// blit2d.h — module interface (Wave-D stream D.5: blit2d). Orthodox C++ (POD +
// free functions, C headers, C-style casts, errno-like returns; no STL / class
// / template / exception / auto / lambda / new / reference-param / overload /
// default-arg / enum-class / namespace).
//
// Screen-space 2D background blits for the N64 sky, run FULL-FRAME as the
// background BEFORE the 3D sweep. No depth; does not seed the zbuf. Output is
// native-endian RGB565 (gfx/framebuffer.h color_t / rgb565()), byte-compatible
// with the ST7789 path (PIO MSB-first, DMA bswap=false, COLMOD 0x55).
//
// Two modes, one POD descriptor (Blit2dRect):
//   BLIT2D_PANORAMA: a scrolling cylinder assembled from a CI8 source window +
//     RGBA5551 TLUT. The src_w-wide (e.g. 512) source wraps horizontally into
//     the dst rect; a vertical band (elevation-driven) selects the source rows.
//     CI / palette is POINT-sampled (no bilinear on indices — N64 RDP CI law).
//   BLIT2D_CLOUDS: an I8 intensity used as ALPHA, blending cloud_color over a
//     procedural vertical blue gradient (top -> horizon) offset by horizon_row.
//
// 1:1 re-aspect: the panel is 240x240 (square), but N64 source art is authored
// for the 4:3 320x240 framebuffer. The vertical horizon mapping is recomputed
// for 1:1 (see blit2d_horizon_row_1to1) so the 2D sky meets the 3D terrain
// horizon WITHOUT a seam — the 3D pass and this blit share the same horizon
// row.
//
// Sampling note (T3): this module ships a SELF-CONTAINED minimal CI8/I8 decode
// (approach (b), for independence from the concurrently-landing tex module).
// tex.h's tex_sample() on main supports only RGBA565/4444 (no CI/I yet), so it
// cannot be reused here today. TODO(T3): once tex_sample handles CI8+TLUT and
// I8, swap blit2d_decode_ci8 / blit2d_decode_i8 for tex_sample (point filter).
#ifndef RDR_BLIT2D_H
#define RDR_BLIT2D_H
#include "rdr/types.h"

// Blit modes (valid values for Blit2dRect.mode).
enum Blit2dMode {
  BLIT2D_PANORAMA = 0,  // CI8 + RGBA5551 TLUT scrolling cylinder
  BLIT2D_CLOUDS = 1     // I8 intensity-as-alpha over a procedural blue gradient
};

// Screen-space 2D blit descriptor (POD). Field meaning depends on `mode`.
// Coordinates are integers (pixels for dst, texels for src); scroll stepping is
// integer/fixed (floats only in setup, never per-pixel).
struct Blit2dRect {
  uint8_t mode;  // enum Blit2dMode

  // --- source window (PANORAMA: CI8 indices; CLOUDS: I8 intensity) ---------
  const void* src;  // CI8 (uint8_t indices) or I8 (uint8_t intensity), null-ok
  const void*
      tlut;  // PANORAMA: RGBA5551 uint16_t[256] palette; CLOUDS: ignored
  uint16_t src_w, src_h;  // source dims in texels (PANORAMA src_w == cylinder
                          // circumference, e.g. 512). pow2 not required.

  // --- destination rect (clamped to the framebuffer) -----------------------
  int16_t dst_x, dst_y;   // top-left of the blit in screen pixels
  uint16_t dst_w, dst_h;  // blit size in screen pixels

  // --- scroll / elevation (PANORAMA) ---------------------------------------
  uint16_t scroll_x;  // horizontal source offset in texels (wraps mod src_w)
  int16_t elevation;  // vertical band offset in source texels (camera pitch);
                      // selects which source rows map to the dst band.

  // --- horizon (both modes) ------------------------------------------------
  int16_t
      horizon_row;  // dst row (screen Y) where the sky meets the 3D horizon;
                    // shared with the 3D pass so the seam disappears.

  // --- CLOUDS gradient + cloud color ---------------------------------------
  uint16_t sky_top;      // RGB565 sky color at dst_y (top of the gradient)
  uint16_t sky_horizon;  // RGB565 sky color at horizon_row (bottom of gradient)
  uint16_t cloud_color;  // RGB565 cloud color blended in by the I8 alpha
};

// Recompute the seam-free horizon row for the 1:1 (240x240) panel from the
// authored 4:3 source. `src_horizon_row` is the horizon's row in the SOURCE
// art (0..src_h-1); `src_h` is the authored source height; `dst_h` is the
// destination band height in screen pixels (typically RDR_SCREEN_H == 240).
// Returns the dst row (0..dst_h-1) the source horizon maps to under the 1:1
// vertical rescale. Setup-only (float); not called per pixel.
int blit2d_horizon_row_1to1(int src_horizon_row, int src_h, int dst_h);

// Decode one CI8 texel: fetch the 8-bit index from `ci8` at (x,y) (row-major,
// width `w`), then the RGBA5551 palette entry from `tlut` (256 uint16_t), and
// return it converted to packed RGB565 (alpha bit dropped — the 565 target is
// opaque). POINT sample (no index bilinear). Null src/tlut returns 0.
uint16_t blit2d_decode_ci8(const void* ci8, const void* tlut, int w, int x,
                           int y);

// Decode one I8 texel: return the raw 8-bit intensity at (x,y) (row-major,
// width `w`) in `i8` as 0..255. Used as the cloud ALPHA. Null src returns 0.
uint8_t blit2d_decode_i8(const void* i8, int w, int x, int y);

// Render a single panorama blit (BLIT2D_PANORAMA) into the full-screen RGB565
// framebuffer `fb` (RDR_SCREEN_W x RDR_SCREEN_H, row-major, native-endian).
// Assembles the scrolling cylinder: dst column c samples source column
// (scroll_x + c) mod src_w; dst row r samples source row clamped by elevation.
// Writes opaque RGB565 (no depth, no zbuf). Returns RDR_EINVAL on a bad
// descriptor (wrong mode / null fb / zero dims), else RDR_OK.
RdrErr blit2d_panorama(const struct Blit2dRect* r, uint16_t* fb);

// Render a single cloud blit (BLIT2D_CLOUDS) into `fb`. For each dst pixel:
// build the procedural blue gradient (sky_top -> sky_horizon, by row relative
// to horizon_row), sample the I8 source as alpha a (0..255), and alpha-over
// cloud_color onto the gradient: out = (a*cloud + (255-a)*gradient + 127)/255
// per channel, packed to RGB565. Returns RDR_EINVAL on a bad descriptor, else
// RDR_OK.
RdrErr blit2d_clouds(const struct Blit2dRect* r, uint16_t* fb);

// Dispatch on r->mode to blit2d_panorama / blit2d_clouds. Returns RDR_EINVAL
// for an unknown mode.
RdrErr blit2d_render(const struct Blit2dRect* r, uint16_t* fb);

#endif  // RDR_BLIT2D_H
