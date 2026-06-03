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
Texture — RGBA565 / RGBA4444 / RGBA5551 / I8 / I4 / IA8 / IA4 / CI8 / CI4
----------------------------------------------------------------------------
RGBA565  (TEXFMT_RGBA565 = 0): bit15..0 = R5 G6 B5
   v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)
   (identical to gfx/framebuffer.h rgb565() and oracle rgb565 — verified.)
RGBA4444 (TEXFMT_RGBA4444 = 1): bit15..0 = R4 G4 B4 A4 (R high nibble)
   v = ((r & 0xF0) << 8) | ((g & 0xF0) << 4) | (b & 0xF0) | (a >> 4)
   (R-first ordering matches the TexFormat enum naming and N64 RGBA16.)

N64 formats (big-endian on N64; converted to little-endian LE by converter):
RGBA5551 (TEXFMT_RGBA5551 = 8): bit15..0 = R5 G5 B5 A1 (N64 RGBA16 native)
   N64 big-endian: hi_byte = R5G5B5A1[15:8], lo_byte = R5G5B5A1[7:0]
   We store LE: lo_byte first, hi_byte second. A1 is bit 0.
I8       (TEXFMT_I8 = 4): one byte per texel = intensity 0..255
I4       (TEXFMT_I4 = 5): two texels per byte, hi nibble first
IA8      (TEXFMT_IA8 = 2): one byte per texel = I4 A4 (hi nibble = intensity)
IA4      (TEXFMT_IA4 = 3): two texels per byte, hi nibble first; per texel I3 A1
CI8      (TEXFMT_CI8 = 6): one byte per texel = palette index 0..255
CI4      (TEXFMT_CI4 = 7): two texels per byte, hi nibble first; per texel idx 0..15
TLUT     (palette for CI formats): 256 entries of RGBA5551 LE uint16
"""

import struct

# ---- TexFormat ids (mirror src/rdr/types.h enum TexFormat) ------------------
TEXFMT_RGBA565 = 0
TEXFMT_RGBA4444 = 1
TEXFMT_IA8 = 2
TEXFMT_IA4 = 3
TEXFMT_I8 = 4
TEXFMT_I4 = 5
TEXFMT_CI4 = 6
TEXFMT_CI8 = 7
TEXFMT_RGBA5551 = 8

# S10.5 fixed-point: 5 fractional bits.
UV_FRAC_BITS = 5
UV_SCALE = 1 << UV_FRAC_BITS  # 32
INT16_MIN = -32768
INT16_MAX = 32767

# Global pre-scale: applied to ALL positional/camera/fog data at convert time.
# Keeps Q16.16 transform inside |A*B| < 2^15.
TERRAIN_PRE_SCALE = 0.005


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


def pack_rgba5551(r, g, b, a):
    """Pack 8-bit RGBA into RGBA5551 (R high, A1 low bit).

    N64 native format. The 1-bit alpha is a simple >= 128 threshold.
    """
    a1 = 1 if a >= 128 else 0
    return (((r & 0xF8) << 8) | ((g & 0xF8) << 3) | ((b & 0xF8) >> 2) | a1) & 0xFFFF


def unpack_rgba5551_be(hi, lo):
    """Unpack big-endian RGBA5551 (N64 on-disk) to (r8, g8, b8, a8).

    hi/lo are the two raw bytes from N64 memory (big-endian u16).
    Returns 8-bit components with 5-bit values expanded via (v*255+15)//31.
    """
    v = (hi << 8) | lo
    r5 = (v >> 11) & 0x1F
    g5 = (v >> 6) & 0x1F
    b5 = (v >> 1) & 0x1F
    a1 = v & 0x1
    r8 = (r5 * 255 + 15) // 31 if r5 else 0
    g8 = (g5 * 255 + 15) // 31 if g5 else 0
    b8 = (b5 * 255 + 15) // 31 if b5 else 0
    a8 = 255 if a1 else 0
    return r8, g8, b8, a8


def n64_rgba5551_be_to_le_blob(raw_bytes):
    """Byte-swap a big-endian N64 RGBA5551 blob to little-endian.

    raw_bytes: bytes object, length must be even (pairs of bytes per texel).
    Each u16 big-endian (hi,lo) becomes LE (lo,hi). No format conversion —
    just the byte order flip required by our little-endian sampler.
    """
    if len(raw_bytes) % 2 != 0:
        raise ValueError('RGBA5551 blob length must be even, got %d' % len(raw_bytes))
    out = bytearray(len(raw_bytes))
    for i in range(0, len(raw_bytes), 2):
        out[i] = raw_bytes[i + 1]    # lo byte first (LE)
        out[i + 1] = raw_bytes[i]    # hi byte second
    return bytes(out)


def encode_i8_blob(intensities):
    """Encode a list of 0..255 intensity values as raw I8 bytes."""
    out = bytearray(len(intensities))
    for i in range(len(intensities)):
        out[i] = intensities[i] & 0xFF
    return bytes(out)


def encode_i4_blob(intensities):
    """Encode a list of 0..255 intensity values as I4 (two per byte, hi nibble first).

    Length must be even. Each value is right-shifted to 4 bits.
    """
    if len(intensities) % 2 != 0:
        raise ValueError('I4 count must be even, got %d' % len(intensities))
    out = bytearray(len(intensities) // 2)
    for i in range(0, len(intensities), 2):
        hi = (intensities[i] >> 4) & 0xF
        lo = (intensities[i + 1] >> 4) & 0xF
        out[i // 2] = (hi << 4) | lo
    return bytes(out)


def encode_ia8_blob(ia_pairs):
    """Encode a list of (intensity, alpha) 0..255 pairs as IA8 (I4 A4 per byte).

    Each byte: hi nibble = intensity>>4, lo nibble = alpha>>4.
    """
    out = bytearray(len(ia_pairs))
    for i in range(len(ia_pairs)):
        intensity, alpha = ia_pairs[i]
        out[i] = ((intensity >> 4) & 0xF) << 4 | ((alpha >> 4) & 0xF)
    return bytes(out)


def encode_ia4_blob(ia_pairs):
    """Encode a list of (intensity, alpha) pairs as IA4 (two per byte, hi nibble first).

    Per texel: I3 A1 packed in a nibble. hi nibble = first texel, lo = second.
    Length must be even.
    """
    if len(ia_pairs) % 2 != 0:
        raise ValueError('IA4 count must be even, got %d' % len(ia_pairs))
    out = bytearray(len(ia_pairs) // 2)
    for i in range(0, len(ia_pairs), 2):
        i0, a0 = ia_pairs[i]
        i1, a1 = ia_pairs[i + 1]
        # I3 = top 3 bits of intensity, A1 = top bit of alpha
        nib0 = (((i0 >> 5) & 0x7) << 1) | (1 if a0 >= 128 else 0)
        nib1 = (((i1 >> 5) & 0x7) << 1) | (1 if a1 >= 128 else 0)
        out[i // 2] = (nib0 << 4) | nib1
    return bytes(out)


def encode_ci8_blob(indices):
    """Encode a list of 0..255 palette indices as CI8 (one byte per texel)."""
    out = bytearray(len(indices))
    for i in range(len(indices)):
        out[i] = indices[i] & 0xFF
    return bytes(out)


def encode_ci4_blob(indices):
    """Encode a list of 0..15 palette indices as CI4 (two per byte, hi nibble first).

    Length must be even.
    """
    if len(indices) % 2 != 0:
        raise ValueError('CI4 count must be even, got %d' % len(indices))
    out = bytearray(len(indices) // 2)
    for i in range(0, len(indices), 2):
        hi = indices[i] & 0xF
        lo = indices[i + 1] & 0xF
        out[i // 2] = (hi << 4) | lo
    return bytes(out)


def encode_tlut_rgba5551(rgba_list):
    """Encode a TLUT (palette) as little-endian RGBA5551 uint16 entries.

    rgba_list: list of (r, g, b, a) 8-bit tuples (up to 256 entries).
    Returns bytes of len(rgba_list)*2.
    """
    out = bytearray()
    for r, g, b, a in rgba_list:
        v = pack_rgba5551(r, g, b, a)
        out += struct.pack('<H', v)
    return bytes(out)


def decode_tlut_from_n64_blob(blob, num_entries):
    """Parse num_entries big-endian RGBA5551 u16s from N64 raw blob.

    Returns a list of (r8, g8, b8, a8) tuples. blob offset is 0.
    """
    if len(blob) < num_entries * 2:
        raise ValueError('TLUT blob too small: need %d bytes, got %d'
                         % (num_entries * 2, len(blob)))
    entries = []
    for i in range(num_entries):
        hi = blob[i * 2]
        lo = blob[i * 2 + 1]
        entries.append(unpack_rgba5551_be(hi, lo))
    return entries


def n64_ci8_blob_to_indices(raw_bytes):
    """Extract CI8 index list from raw N64 bytes (no byte-swap needed: 1 byte/texel)."""
    return list(raw_bytes)


def n64_ci4_blob_to_indices(raw_bytes, count):
    """Extract CI4 index list from raw N64 bytes (hi nibble first, no byte-swap)."""
    indices = []
    for i in range(len(raw_bytes)):
        indices.append((raw_bytes[i] >> 4) & 0xF)
        indices.append(raw_bytes[i] & 0xF)
        if len(indices) >= count:
            break
    return indices[:count]


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


# ---- Terrain grid parser -------------------------------------------------------

def parse_n64_terrain_grid_c(src_text):
    """Parse N64 terrain_grid.c C source into a list of vertex dicts.

    Returns list of {'pos': (x,y,z), 'uv': (s,t), 'rgba': (r,g,b,a)} dicts
    for all 594 entries (including zero-padded ones at end).
    The UVs are the raw S10.5 integer values from the source (not scaled).
    """
    import re
    verts = []
    # Pattern: {{ {x, y, z}, flag, {s, t}, {r, g, b, a} }}
    pat = re.compile(
        r'\{\{\s*\{([^}]+)\}\s*,\s*\d+\s*,\s*\{([^}]+)\}\s*,\s*\{([^}]+)\}\s*\}\}'
    )
    for m in pat.finditer(src_text):
        pos = tuple(int(v.strip()) for v in m.group(1).split(','))
        uv = tuple(int(v.strip()) for v in m.group(2).split(','))
        rgba = tuple(int(v.strip()) for v in m.group(3).split(','))
        verts.append({'pos': pos, 'uv': uv, 'rgba': rgba})
    return verts


def parse_n64_hex_array_c(src_text, dtype='u16'):
    """Parse a C array of hex literals (0x...) from N64 source into a list of ints.

    dtype: 'u16' parses as uint16, 'u8' parses as uint8.
    Returns a flat list of ints.
    """
    import re
    values = [int(x, 16) for x in re.findall(r'0x[0-9a-fA-F]+', src_text)]
    if dtype == 'u16':
        for v in values:
            if v > 0xFFFF:
                raise ValueError('u16 value out of range: %d' % v)
    elif dtype == 'u8':
        for v in values:
            if v > 0xFF:
                raise ValueError('u8 value out of range: %d' % v)
    return values


def parse_n64_scenery_cylinders_c(src_text):
    """Parse scenery_cylinders.c into a list of instance dicts.

    Each entry: {'cx': int, 'y': int, 'cz': int, 'sprite_id': int,
                 'visibility': int, 'radius': int, 'height': int}.
    Entries with sprite_id < 0 are dead slots (skipped).
    """
    import re
    entries = []
    # Pattern: { cx, y, cz, sprite_id, visibility, radius, height }
    pat = re.compile(r'\{\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,'
                     r'\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*,\s*(-?\d+)\s*\}')
    for m in pat.finditer(src_text):
        vals = [int(v) for v in m.groups()]
        entries.append({
            'cx': vals[0], 'y': vals[1], 'cz': vals[2],
            'sprite_id': vals[3], 'visibility': vals[4],
            'radius': vals[5], 'height': vals[6],
        })
    return entries


# ---- Terrain grid baking -------------------------------------------------------

def grid_index(col, row):
    """Map fine-lattice cell (col 0..16, row 0..32) to g_terrain_grid index.

    Even cols: row*17 + col/2.  Odd cols: row*17 + 9 + (col-1)/2.
    Mirrors terrain_index.h grid_index().
    """
    base = row * 17
    if (col & 1) == 0:
        return base + (col >> 1)
    return base + 9 + ((col - 1) >> 1)


def tile_vert_indices_terrain(tx, ty):
    """Return list of 9 grid indices for tile (tx,ty), row-major 3x3 block.

    Mirrors terrain_index.h tile_vert_indices().
    """
    out = []
    col0 = tx * 2
    row0 = ty * 2
    for cy in range(3):
        for cx in range(3):
            out.append(grid_index(col0 + cx, row0 + cy))
    return out


# In-tile gutter offset: content starts at col 2 (2-col left gutter) and row 1
# (1-row top gutter) within the 36x34 block.
TILE_GUTTER_S = 2
TILE_GUTTER_T = 1
TILE_W = 36
TILE_H = 34
TERRAIN_TILES_X = 8   # N64 tile grid columns
TERRAIN_TILES_Y = 16  # N64 tile grid rows


def terrain_tile_in_tile_uv(grid_vert, tx, ty):
    """Compute one vertex's in-tile texel UV within the 36x34 gutter'd block.

    grid_vert: a single dict {'pos','uv','rgba'} from parse_n64_terrain_grid_c().
    Returns (s_texel, t_texel) in [TILE_GUTTER_S, TILE_W-TILE_GUTTER_S] x
    [TILE_GUTTER_T, TILE_H-TILE_GUTTER_T] = [2,34] x [1,33].

    UV math (per terrain.c gDPSetTileSize windowing):
      gSPTexture scale 0.5 -> final_s = global_S * 0.5 (S10.5 units).
      uls_s10_5 = (tx*0x80 + 2) * 8 (the tile's TMEM window origin, S10.5).
      s_tile_s10_5 = final_s - uls_s10_5; in texels = /32; add the gutter offset
      so the content edge lands at col 2 (left gutter replicated to cols 0-1).
    """
    sx, sy = grid_vert['uv']
    uls_s10_5 = (tx * 0x80 + 2) * 8
    ult_s10_5 = (ty * 0x80 + 2) * 8
    final_s = sx / 2.0
    final_t = sy / 2.0
    s_tile = final_s - uls_s10_5
    t_tile = final_t - ult_s10_5
    s_texel = s_tile / 32.0 + TILE_GUTTER_S
    t_texel = t_tile / 32.0 + TILE_GUTTER_T
    return s_texel, t_texel


def bake_terrain_tile_vertices(grid_verts, tx, ty, pre_scale):
    """Bake 9 per-tile vertices for tile (tx,ty) with in-tile UV within 36x34 block.

    grid_verts: list of 594 dicts from parse_n64_terrain_grid_c().
    pre_scale:  float, applied to all positional coords (e.g. 0.005).
    Returns list of 9 (pos, uv, rgba) tuples ready for encode_vertex_prelit().

    UVs are LOCAL to the 36x34 block ([2,34]x[1,33]); for the single-atlas bake,
    add the tile's atlas origin (see bake_terrain_atlas_vertices). This per-tile
    form is retained for the fallback per-tile-texture path and the unit tests.
    """
    indices = tile_vert_indices_terrain(tx, ty)
    out = []
    for idx in indices:
        g = grid_verts[idx]
        x, y, z = g['pos']
        r, gb, b, a = g['rgba']

        # Apply pre-scale to positional coordinates
        px = round_half_away(x * pre_scale)
        py = round_half_away(y * pre_scale)
        pz = round_half_away(z * pre_scale)

        s_texel, t_texel = terrain_tile_in_tile_uv(g, tx, ty)
        out.append(((px, py, pz), (s_texel, t_texel), (r, gb, b, a)))
    return out


# ---- Single gutter'd atlas (PRIMARY terrain texture path) ----------------------
#
# Atlas cell = the 36x34 source block PLUS a 1-row replicated bottom spacer, so
# the cell stride is 36 wide x 35 tall. WHY the spacer: the N64 source block has a
# 2-col horizontal gutter (cols 0,1 and 34,35) but only a 1-row vertical gutter
# (rows 0 and 33). The far-edge vertices land at the gutter texels (col 34, row
# 33). A bilinear tap there reads {col 34, col 35} horizontally (both inside the
# 36-wide block -> safe) and {row 33, row 34} vertically. Row 34 would be the TOP
# row of the next tile stacked below in the atlas -> seam bleed. The spacer row 34
# is a replicated copy of row 33, so the bilinear tap reads only this tile's data.
# (N64 avoided this with cmt=CLAMP; our flat atlas needs the physical spacer.)
ATLAS_CELL_W = TILE_W       # 36 (the 2-col gutter already makes col 34/35 safe)
ATLAS_CELL_H = TILE_H + 1   # 35 (34 block + 1 replicated bottom spacer row)


def atlas_tile_origin(tile_idx, tiles_per_row):
    """Return the (col_texel, row_texel) atlas origin of tile tile_idx.

    Tiles pack left-to-right, top-to-bottom: tile t at grid (t % tiles_per_row,
    t // tiles_per_row), origin = (col*ATLAS_CELL_W, row*ATLAS_CELL_H) texels.
    """
    col = tile_idx % tiles_per_row
    row = tile_idx // tiles_per_row
    return col * ATLAS_CELL_W, row * ATLAS_CELL_H


def terrain_atlas_dims(num_tiles, atlas_w):
    """Compute (tiles_per_row, rows, atlas_h_pow2) for a pow2-wide atlas.

    atlas_w must be pow2 and >= ATLAS_CELL_W. Packs num_tiles into rows of
    tiles_per_row = atlas_w // ATLAS_CELL_W; atlas_h is the next pow2 >=
    rows*ATLAS_CELL_H. Raises ValueError if atlas_w is not pow2 or too small.
    """
    import math
    if atlas_w < ATLAS_CELL_W or (atlas_w & (atlas_w - 1)) != 0:
        raise ValueError('atlas_w must be pow2 and >= %d, got %d'
                         % (ATLAS_CELL_W, atlas_w))
    tiles_per_row = atlas_w // ATLAS_CELL_W
    rows = (num_tiles + tiles_per_row - 1) // tiles_per_row
    content_h = rows * ATLAS_CELL_H
    atlas_h = 1 << math.ceil(math.log2(content_h))
    return tiles_per_row, rows, atlas_h


def build_terrain_atlas_rgba5551_le(tex_values, num_tiles, atlas_w, atlas_h,
                                    tiles_per_row):
    """Build a single gutter'd atlas image as little-endian RGBA5551 bytes.

    tex_values: flat list of uint16 (big-endian N64 RGBA5551), 128*36*34 long.
    Each tile's 36x34 block is copied to its atlas origin; row 34 of the cell is a
    replicated copy of the block's bottom row (33) -> bilinear at the bottom edge
    never reads the next stacked tile. Uncovered atlas areas are zero. Returns
    bytes of length atlas_w * atlas_h * 2.
    """
    out = bytearray(atlas_w * atlas_h * 2)  # zero-initialized
    texels_per_tile = TILE_W * TILE_H
    for t in range(num_tiles):
        base = t * texels_per_tile
        ox, oy = atlas_tile_origin(t, tiles_per_row)
        for ty_in in range(ATLAS_CELL_H):
            # Spacer row (ty_in == TILE_H) replicates the block's last row.
            src_row = ty_in if ty_in < TILE_H else TILE_H - 1
            for tx_in in range(TILE_W):
                v = tex_values[base + src_row * TILE_W + tx_in]
                hi = (v >> 8) & 0xFF
                lo = v & 0xFF
                ax = ox + tx_in
                ay = oy + ty_in
                off = (ay * atlas_w + ax) * 2
                out[off] = lo       # LE: lo first
                out[off + 1] = hi   # hi second
    return bytes(out)


def bake_terrain_atlas_vertices(grid_verts, atlas_w, atlas_h, tiles_per_row,
                                pre_scale):
    """Bake all 128*9 terrain vertices with ABSOLUTE atlas UV coordinates.

    For each tile (tx,ty) -> linear tile index t = ty*TERRAIN_TILES_X + tx,
    each of its 9 vertices gets UV = (atlas_origin) + (in-tile [2,34]x[1,33]).
    Positions are pre-scaled. Returns a flat list of 128*9 (pos, uv, rgba) tuples
    in tile-major, then vertex-row-major order (matches the index pattern).
    """
    out = []
    for ty in range(TERRAIN_TILES_Y):
        for tx in range(TERRAIN_TILES_X):
            t = ty * TERRAIN_TILES_X + tx
            ox, oy = atlas_tile_origin(t, tiles_per_row)
            indices = tile_vert_indices_terrain(tx, ty)
            for idx in indices:
                g = grid_verts[idx]
                x, y, z = g['pos']
                r, gb, b, a = g['rgba']
                px = round_half_away(x * pre_scale)
                py = round_half_away(y * pre_scale)
                pz = round_half_away(z * pre_scale)
                s_local, t_local = terrain_tile_in_tile_uv(g, tx, ty)
                s_atlas = ox + s_local
                t_atlas = oy + t_local
                out.append(((px, py, pz), (s_atlas, t_atlas), (r, gb, b, a)))
    return out


def bake_terrain_tile_indices():
    """Return the 8-triangle index pattern for a 3x3 vertex block (per-tile).

    9 vertices in row-major order (local idx = cy*3 + cx).
    2x2 quads, each split into 2 triangles (NW,NE,SW + NE,SE,SW).
    Returns a flat list of 24 uint16 indices (8 tris * 3 verts).
    """
    indices = []
    for qy in range(2):
        for qx in range(2):
            nw = qy * 3 + qx
            ne = nw + 1
            sw = nw + 3
            se = sw + 1
            # Tri1: NW, NE, SW (CCW winding matching N64 G_CULL_BACK)
            indices += [nw, ne, sw]
            # Tri2: NE, SE, SW
            indices += [ne, se, sw]
    return indices


def extract_tile_texture_pixels(tex_values, tile_idx, tile_w, tile_h):
    """Extract one tile's RGBA5551 big-endian values from the flat terrain texture array.

    tex_values: flat list of uint16 values (big-endian RGBA5551, from N64 C array).
    tile_idx:   0..127 tile index.
    tile_w:     36 (columns per tile).
    tile_h:     34 (rows per tile).
    Returns a list of tile_w*tile_h (r8,g8,b8,a8) tuples decoded from RGBA5551.
    """
    texels_per_tile = tile_w * tile_h
    base = tile_idx * texels_per_tile
    pixels = []
    for i in range(texels_per_tile):
        v = tex_values[base + i]
        hi = (v >> 8) & 0xFF
        lo = v & 0xFF
        r8, g8, b8, a8 = unpack_rgba5551_be(hi, lo)
        pixels.append((r8, g8, b8, a8))
    return pixels


def encode_terrain_tile_rgba5551_le(tex_values, tile_idx, tile_w, tile_h):
    """Encode one terrain tile as little-endian RGBA5551 bytes.

    Byte-swaps each big-endian N64 u16 to little-endian for our sampler.
    tex_values: flat list of uint16 values as parsed from the N64 C array.
    Returns bytes of length tile_w * tile_h * 2.
    """
    texels_per_tile = tile_w * tile_h
    base = tile_idx * texels_per_tile
    out = bytearray()
    for i in range(texels_per_tile):
        v = tex_values[base + i]
        hi = (v >> 8) & 0xFF
        lo = v & 0xFF
        # LE: lo byte first, hi byte second
        out.append(lo)
        out.append(hi)
    return bytes(out)


# ---- matrix bake (TW-05: lock the column-major convention with a guard) -----
# The renderer's mat_transform (src/fixed/fixed.cc) computes
#     out[row] = sum_k m[k*4 + row] * v[k]      (column-major flat array)
# and the N64 produces clip via the row-vector form
#     clip[j]  = sum_i v[i] * vp[i][j].
# Equating out[j] == clip[j] gives the bake convention  m[i*4 + j] = vp[i][j].
# The S0 spike hit the SILENT wrong-w bug from baking the transpose
# (m[i*4+j] = vp[j][i]); D.1/D.7 must bake scripted camera + asset MVPs through
# THIS helper so asset_tool_test pins the convention.
FX16_16_ONE = 1 << 16
INT32_MIN = -(1 << 31)
INT32_MAX = (1 << 31) - 1


def to_q16_16(x):
    """Float -> Q16.16 int (round half away from zero), clamped to int32."""
    v = round_half_away(x * FX16_16_ONE)
    if v < INT32_MIN:
        return INT32_MIN
    if v > INT32_MAX:
        return INT32_MAX
    return v


def bake_mvp_q16_16(vp):
    """Bake a 4x4 V*P (N64 row-vector convention vp[i][j]) into the flat Q16.16
    array the renderer consumes: flat[i*4 + j] = q16(vp[i][j]). NOT the transpose.
    vp: 4x4 nested sequence of floats. Returns a flat list of 16 Q16.16 ints."""
    if len(vp) != 4 or any(len(row) != 4 for row in vp):
        raise ValueError('vp must be 4x4')
    return [to_q16_16(vp[i][j]) for i in range(4) for j in range(4)]


def transform_rowvec_ref(vp, v):
    """N64 reference (float): clip[j] = sum_i v[i]*vp[i][j]. v is length-4."""
    return [sum(v[i] * vp[i][j] for i in range(4)) for j in range(4)]


def transform_renderer_model(m_flat, v):
    """Float model of the renderer's mat_transform: out[row] = sum_k
    m_flat[k*4+row]*v[k]. m_flat is the Q16.16 flat array; v is length-4."""
    return [sum((m_flat[k * 4 + row] / FX16_16_ONE) * v[k] for k in range(4))
            for row in range(4)]
