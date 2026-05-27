#!/usr/bin/env python3
"""asset_convert.py — offline 3D-renderer asset converter (pure byte layout).

Emits EXACTLY the frozen byte layout from src/rdr/types.h. No invented format.
This module is the testable core: pure functions over plain Python lists, no
PIL/file I/O, so round-trip tests can assert exact output bytes. PNG loading
and CLI live in asset_tool.py.

----------------------------------------------------------------------------
Vertex — struct Vtx (rdr/types.h, GBI Vtx-union style, 16 bytes, little-endian)
----------------------------------------------------------------------------
  offset size  field            maps to types.h
  0      2     pos[0]  int16    Vtx.pos[0]   model-space x
  2      2     pos[1]  int16    Vtx.pos[1]   model-space y
  4      2     pos[2]  int16    Vtx.pos[2]   model-space z
  6      2     uv[0]   int16    Vtx.uv[0]    S10.5 texcoord s
  8      2     uv[1]   int16    Vtx.uv[1]    S10.5 texcoord t
  10     1     c.rgba[0]/c.nrm.n[0]   union arm 0/lit byte 0
  11     1     c.rgba[1]/c.nrm.n[1]   union arm 0/lit byte 1
  12     1     c.rgba[2]/c.nrm.n[2]   union arm 0/lit byte 2
  13     1     c.rgba[3]/c.nrm.a      union arm 0/lit byte 3
  (struct is 14 bytes of fields; C aligns Vtx to 2 (max member int16) -> 14;
   no trailing pad beyond 14. Total = 14 bytes. Verified by static_assert
   in asset_layout_test against the C compiler.)

NOTE: although the header comment says "16 B", the actual POD size with int16
alignment is 14 bytes (3*2 + 2*2 + 4*1). The exporter emits the true compiler
layout; the C-side static_assert is the authority and is checked by the test.

UV is S10.5 fixed-point: texel coordinate * 32, rounded to nearest, clamped to
int16 range. (S10.5 -> 10 integer bits + 5 fractional, sign => 16-bit signed.)

----------------------------------------------------------------------------
Texture — RGBA565 / RGBA4444 (TexFormat enum), little-endian uint16 per texel
----------------------------------------------------------------------------
RGBA565  (TEXFMT_RGBA565 = 0): bit15..0 = R5 G6 B5
   v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
   (identical to gfx/framebuffer.h rgb565() and oracle rgb565 — verified.)
RGBA4444 (TEXFMT_RGBA4444 = 1): bit15..0 = R4 G4 B4 A4 (R high nibble)
   v = ((r & 0xF0) << 8) | ((g & 0xF0) << 4) | (b & 0xF0) | (a >> 4)
   (R-first ordering matches the TexFormat enum naming and N64 RGBA16.)
"""

import struct

# ---- TexFormat ids (mirror src/rdr/types.h enum TexFormat) ------------------
TEXFMT_RGBA565 = 0
TEXFMT_RGBA4444 = 1

# S10.5 fixed-point: 5 fractional bits.
UV_FRAC_BITS = 5
UV_SCALE = 1 << UV_FRAC_BITS  # 32
INT16_MIN = -32768
INT16_MAX = 32767


def clamp_i16(v):
    """Clamp an integer to signed 16-bit range."""
    if v < INT16_MIN:
        return INT16_MIN
    if v > INT16_MAX:
        return INT16_MAX
    return v


def round_half_away(x):
    """Round to nearest, ties away from zero (matches C lroundf semantics)."""
    if x >= 0.0:
        return int(x + 0.5)
    return -int(-x + 0.5)


def uv_to_s10_5(coord):
    """Convert a float texel coordinate to an S10.5 int16."""
    return clamp_i16(round_half_away(coord * UV_SCALE))


def pack_rgb565(r, g, b):
    """Pack 8-bit RGB into RGB565 (R high). Matches framebuffer.h rgb565()."""
    return (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)) & 0xFFFF


def pack_rgba4444(r, g, b, a):
    """Pack 8-bit RGBA into RGBA4444 (R high nibble, A low nibble)."""
    return (((r & 0xF0) << 8) | ((g & 0xF0) << 4) | (b & 0xF0) | (a >> 4)) & 0xFFFF


def encode_vertex_prelit(pos, uv, rgba):
    """Encode one pre-lit vertex to its 14-byte little-endian struct Vtx image.

    pos:  (x, y, z) ints (model-space, already in renderer integer units)
    uv:   (s, t) floats in texel coords (converted to S10.5)
    rgba: (r, g, b, a) 0..255 ints
    """
    s = uv_to_s10_5(uv[0])
    t = uv_to_s10_5(uv[1])
    return struct.pack(
        '<hhh' 'hh' 'BBBB',
        clamp_i16(int(pos[0])), clamp_i16(int(pos[1])), clamp_i16(int(pos[2])),
        s, t,
        rgba[0] & 0xFF, rgba[1] & 0xFF, rgba[2] & 0xFF, rgba[3] & 0xFF,
    )


def encode_vertex_lit(pos, uv, normal, alpha):
    """Encode one lit vertex (normal+alpha union arm) to its 14-byte image.

    normal: (nx, ny, nz) signed bytes -128..127; alpha: 0..255.
    """
    s = uv_to_s10_5(uv[0])
    t = uv_to_s10_5(uv[1])
    return struct.pack(
        '<hhh' 'hh' 'bbb' 'B',
        clamp_i16(int(pos[0])), clamp_i16(int(pos[1])), clamp_i16(int(pos[2])),
        s, t,
        normal[0], normal[1], normal[2],
        alpha & 0xFF,
    )


def encode_vertices(verts, lit):
    """Concatenate encoded vertices into one bytes object.

    verts: list of (pos, uv, c) where c is rgba tuple (pre-lit) or
           (normal_tuple, alpha) (lit). 'lit' selects the union arm.
    """
    out = bytearray()
    for pos, uv, c in verts:
        if lit:
            out += encode_vertex_lit(pos, uv, c[0], c[1])
        else:
            out += encode_vertex_prelit(pos, uv, c)
    return bytes(out)


def encode_indices(indices):
    """Encode a triangle index list as little-endian uint16 (matches DRAW_TRIS

    idx[] of uint16). Length must be a multiple of 3 (whole triangles)."""
    if len(indices) % 3 != 0:
        raise ValueError('index count %d not a multiple of 3' % len(indices))
    return struct.pack('<%uH' % len(indices), *(i & 0xFFFF for i in indices))


def encode_texture(pixels, fmt):
    """Encode RGBA pixels (list of (r,g,b,a)) to packed texels, little-endian.

    fmt: TEXFMT_RGBA565 or TEXFMT_RGBA4444. Row-major as supplied by caller."""
    out = bytearray()
    for r, g, b, a in pixels:
        if fmt == TEXFMT_RGBA565:
            v = pack_rgb565(r, g, b)
        elif fmt == TEXFMT_RGBA4444:
            v = pack_rgba4444(r, g, b, a)
        else:
            raise ValueError('unsupported TexFormat %r' % fmt)
        out += struct.pack('<H', v)
    return bytes(out)
