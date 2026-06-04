#!/usr/bin/env python3
"""gen_debug_terrain.py — OFFLINE bake of the D.7/T0 terrain + tree billboard
INPUT geometry to a committed FLASH-CONST header (src/demo/debug_terrain_gen.h).

WHY (integration gate, target-SRAM only): the demo's input geometry must not
live in .bss (RAM) — at ~1152 terrain verts + 504 tree verts + indices it would
overflow the RP2040 RAM region. It is static deterministic data, so bake it to
const flash arrays (like camera_path_gen.h / the asset_gen headers); the demo
submits LOAD_VERTS/DRAW_TRIS pointers straight at the flash arrays.

T0 SWAP: the terrain is now D.1's REAL baked mesh (assets/terrain/
terrain_grid_vtx.h) — positions + UVs verbatim, with the N64 pre-lit gray
overridden by a DISTINCT debug color per mesh-cell (tile = vert/9, tile-major).
Indices are the shared per-tile pattern (terrain_grid_idx.h) expanded over all
128 tiles (pattern[k] + tile*9). Trees stay procedural upright debug-colored
quads at the real scenery density (126).

This generator reproduces demo_terrain_geometry / demo_tree_geometry in
demo_scene.cc EXACTLY (positions, UVs, per-cell debug colors, indices, winding)
so the FlashConstGeometryMatchesFillApi drift guard + the determinism / debug-
color-histogram / near-plane guards keep passing.

Build does NOT run Python; debug_terrain_gen.h is committed data. Regenerate
when the placeholder layout or the baked mesh changes:

    python3 src/demo/gen_debug_terrain.py src/demo/debug_terrain_gen.h
"""

import os
import re
import struct
import sys

# ---- layout constants (MUST mirror src/demo/demo_scene.{h,cc}) -------------
TERRAIN_TILES = 128          # DEMO_TERRAIN_TILES
VERTS_PER_TILE = 9           # DEMO_TERRAIN_VERTS_PER_TILE
TERRAIN_VERTS = TERRAIN_TILES * VERTS_PER_TILE  # 1152
TREE_COUNT = 126             # DEMO_TREE_COUNT
TERRAIN_BASE_Y = 0           # ground plane Y
VTX_STRIDE = 14              # packed Vtx: pos[3] + uv[2] + rgba[4]

_HERE = os.path.dirname(os.path.abspath(__file__))
_GRID_VTX_H = os.path.join(_HERE, 'assets', 'terrain', 'terrain_grid_vtx.h')
_GRID_IDX_H = os.path.join(_HERE, 'assets', 'terrain', 'terrain_grid_idx.h')


def _parse_hex_bytes(path):
    """Flat list of the 0xNN byte literals in a generated C header."""
    text = open(path).read()
    return [int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b', text)]


def tile_debug_color(tile):
    """Mirror tile_debug_color() in demo_scene.cc (3 saw waves, coprime)."""
    r = 64 + ((tile * 37) & 0xBF)
    g = 64 + (((tile * 53) + 17) & 0xBF)
    b = 64 + (((tile * 61) + 31) & 0xBF)
    return r, g, b


def build_terrain():
    """D.1 baked mesh, debug-recolored per mesh-cell + expanded indices."""
    raw = bytes(_parse_hex_bytes(_GRID_VTX_H))
    n = len(raw) // VTX_STRIDE
    if n != TERRAIN_VERTS:
        sys.stderr.write('WARNING: grid vtx count %d != %d\n'
                         % (n, TERRAIN_VERTS))
    verts = []  # (px,py,pz, u,v, r,g,b)
    for i in range(n):
        px, py, pz, u, v = struct.unpack_from('<hhhhh', raw, i * VTX_STRIDE)
        r, g, b = tile_debug_color(i // VERTS_PER_TILE)
        verts.append((px, py, pz, u, v, r, g, b))

    # Shared per-tile index pattern, expanded with a +tile*9 offset.
    pat_text = open(_GRID_IDX_H).read()
    body = pat_text.split('{', 1)[1].split('}', 1)[0]
    pattern = [int(x, 0) for x in re.findall(r'0x[0-9a-fA-F]+|\d+', body)]
    # The asset pattern is already wound up-facing for our CULL_BACK (the
    # converter's bake_terrain_tile_indices adapts the N64 winding), so expand
    # verbatim: tile t's indices are pattern[k] + t*9.
    idx = []
    for t in range(TERRAIN_TILES):
        vbase = t * VERTS_PER_TILE
        idx += [k + vbase for k in pattern]
    return verts, idx


def build_trees():
    """Mirror demo_tree_geometry() in demo_scene.cc (T1: sprite UVs + gray
    Gouraud gradient; CULL_NONE double-sided)."""
    verts = []
    idx = []
    # Per-corner tree0 sprite UVs (S10.5 = texel*32; 32x64 sprite, v=0 at top)
    # and the N64 gray Gouraud gradient (top 206 / bottom 157).
    corner_uv = ((0, 2048), (1024, 2048), (0, 0), (1024, 0))
    corner_gray = (157, 157, 206, 206)
    for t in range(TREE_COUNT):
        cx = -12 + ((t * 7) % 25)
        cz = -12 + ((t * 11) % 25)
        hw = 2
        ht = 5
        base = t * 4
        corner = ((-hw, 0), (hw, 0), (-hw, ht), (hw, ht))
        for k in range(4):
            gray = corner_gray[k]
            verts.append((cx + corner[k][0], TERRAIN_BASE_Y + corner[k][1], cz,
                          corner_uv[k][0], corner_uv[k][1], gray, gray, gray))
        a, bb, c, d = base + 0, base + 1, base + 2, base + 3
        idx += [a, c, bb, bb, c, d]  # winding kept; CULL_NONE => double-sided
    return verts, idx


def fmt_vtx(v):
    # struct Vtx aggregate init: {{pos}, {uv}, {{rgba}}}. alpha=255.
    px, py, pz, u, vv, r, g, b = v
    return '{{%d, %d, %d}, {%d, %d}, {{%d, %d, %d, 255}}}' % (
        px, py, pz, u, vv, r, g, b)


def emit(name_v, name_i, verts, idx, lines):
    lines.append('static const struct Vtx %s[%d] = {' % (name_v, len(verts)))
    for v in verts:
        lines.append('    %s,' % fmt_vtx(v))
    lines.append('};')
    lines.append('static const uint16_t %s[%d] = {' % (name_i, len(idx)))
    # 12 indices per line for readability.
    for i in range(0, len(idx), 12):
        chunk = ', '.join('%d' % x for x in idx[i:i + 12])
        lines.append('    %s,' % chunk)
    lines.append('};')


def emit_header(path):
    tv, ti = build_terrain()
    rv, ri = build_trees()
    lines = []
    lines.append('// clang-format off')
    lines.append('// debug_terrain_gen.h — GENERATED by src/demo/gen_debug_terrain.py.')
    lines.append('// DO NOT EDIT. Committed FLASH-CONST terrain + tree billboard input')
    lines.append('// geometry for the D.7/T0 demo. Baked OFFLINE so the input geometry')
    lines.append('// leaves .bss (RAM) -> fits the RP2040 RAM region. Terrain = D.1\'s')
    lines.append('// real baked mesh (assets/terrain/terrain_grid_vtx.h) debug-recolored')
    lines.append('// per mesh-cell; trees = procedural upright debug quads. Reproduces')
    lines.append('// demo_terrain_geometry / demo_tree_geometry exactly (positions, UVs,')
    lines.append('// per-cell debug colors, indices, winding). The build does not run')
    lines.append('// Python; committed data.')
    lines.append('// Regenerate: python3 src/demo/gen_debug_terrain.py src/demo/debug_terrain_gen.h')
    lines.append('#ifndef DEMO_DEBUG_TERRAIN_GEN_H')
    lines.append('#define DEMO_DEBUG_TERRAIN_GEN_H')
    lines.append('')
    lines.append('#include "rdr/types.h"')
    lines.append('')
    lines.append('// NOLINTBEGIN (generated geometry: suppress magic-number/naming lint)')
    emit('g_debug_terrain_vtx', 'g_debug_terrain_idx', tv, ti, lines)
    emit('g_debug_tree_vtx', 'g_debug_tree_idx', rv, ri, lines)
    lines.append('// NOLINTEND')
    lines.append('')
    lines.append('#endif  // DEMO_DEBUG_TERRAIN_GEN_H')
    lines.append('// clang-format on')
    lines.append('')
    text = '\n'.join(lines)
    with open(path, 'w') as fh:
        fh.write(text)
    return len(text), len(tv), len(ti), len(rv), len(ri)


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(_HERE,
                                                     'debug_terrain_gen.h')
    size, nv, ni, rv, ri = emit_header(out)
    sys.stderr.write(
        'wrote %s: %d bytes (terrain %dv/%di, trees %dv/%di)\n'
        % (out, size, nv, ni, rv, ri))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
