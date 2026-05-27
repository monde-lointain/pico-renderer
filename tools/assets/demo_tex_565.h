/* GENERATED companion for tools/asset_tool.py output demo_tex_565.c.
 * One RGB565 texture asset for the C1 demo. Row-major, top-left origin,
 * little-endian uint16 per texel (R5 G6 B5, matches gfx/framebuffer.h rgb565()
 * and src/rdr/types.h TEXFMT_RGBA565). Use as TexDesc{ data=g_demo_tex_565,
 * w=16, h=16, format=TEXFMT_RGBA565 }. */
#ifndef TOOLS_ASSETS_DEMO_TEX_565_H
#define TOOLS_ASSETS_DEMO_TEX_565_H

#include <stdint.h>

#define DEMO_TEX_565_W 16
#define DEMO_TEX_565_H 16
#define DEMO_TEX_565_FMT 0 /* TEXFMT_RGBA565 */

extern const uint8_t g_demo_tex_565[];   /* 16*16*2 = 512 bytes */
extern const unsigned g_demo_tex_565_len;

#endif /* TOOLS_ASSETS_DEMO_TEX_565_H */
