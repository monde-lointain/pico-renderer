#!/usr/bin/env python3
"""Round-trip / known-byte tests for the 3D-renderer asset converter.

Run standalone:  python3 tools/asset_tool_test.py
No PIL or pytest required: the testable core (asset_convert) is pure Python over
plain lists; exact output bytes are asserted against hand-computed expectations
and against the C compiler's actual struct Vtx layout.
"""

import os
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import asset_convert as ac  # noqa: E402

SRC_INCLUDE = os.path.normpath(
    os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'src'))

# Path to N64 terrain source (optional — tests that need it skip if missing)
N64_TERRAIN_DIR = os.path.expanduser('~/development/n64/terrain')


class MvpBakeConventionTests(unittest.TestCase):
    """TW-05: lock the column-major MVP-bake convention; catch the transpose."""

    # A deliberately ASYMMETRIC 4x4 (so transpose != original) + asymmetric vec.
    VP = [
        [1.0, 2.0, 3.0, 4.0],
        [5.0, 6.0, 7.0, 8.0],
        [9.0, 10.0, 11.0, 12.0],
        [13.0, 14.0, 15.0, 16.0],
    ]
    V = [0.5, -1.5, 2.0, 1.0]

    def test_bake_then_renderer_model_equals_rowvec_reference(self):
        # The contract: feeding the baked array through the renderer's read
        # pattern reproduces the N64 row-vector transform.
        baked = ac.bake_mvp_q16_16(self.VP)
        got = ac.transform_renderer_model(baked, self.V)
        ref = ac.transform_rowvec_ref(self.VP, self.V)
        for g, r in zip(got, ref):
            self.assertAlmostEqual(g, r, places=3)

    def test_transpose_bake_diverges(self):
        # Teeth: the WRONG bake (transpose, flat[i*4+j]=vp[j][i]) must NOT
        # reproduce the reference — otherwise the guard is vacuous.
        transposed = [ac.to_q16_16(self.VP[j][i])
                      for i in range(4) for j in range(4)]
        got = ac.transform_renderer_model(transposed, self.V)
        ref = ac.transform_rowvec_ref(self.VP, self.V)
        self.assertNotAlmostEqual(got[0], ref[0], places=3)

    def test_flatten_is_row_major_iijj(self):
        baked = ac.bake_mvp_q16_16(self.VP)
        self.assertEqual(baked[0 * 4 + 1], ac.to_q16_16(2.0))   # vp[0][1]
        self.assertEqual(baked[2 * 4 + 3], ac.to_q16_16(12.0))  # vp[2][3]

    def test_rejects_non_4x4(self):
        with self.assertRaises(ValueError):
            ac.bake_mvp_q16_16([[1.0, 2.0], [3.0, 4.0]])


class TexturePackingTests(unittest.TestCase):
    def test_rgb565_matches_framebuffer_formula(self):
        # framebuffer.h rgb565: ((r&0xF8)<<8)|((g&0xFC)<<3)|(b>>3)
        self.assertEqual(ac.pack_rgb565(0, 0, 0), 0x0000)
        self.assertEqual(ac.pack_rgb565(255, 255, 255), 0xFFFF)
        self.assertEqual(ac.pack_rgb565(255, 0, 0), 0xF800)
        self.assertEqual(ac.pack_rgb565(0, 255, 0), 0x07E0)
        self.assertEqual(ac.pack_rgb565(0, 0, 255), 0x001F)
        # bit-truncation, not rounding (matches the renderer's masking)
        self.assertEqual(ac.pack_rgb565(20, 30, 60), ((20 & 0xF8) << 8) |
                         ((30 & 0xFC) << 3) | (60 >> 3))

    def test_rgba4444_r_high_a_low(self):
        self.assertEqual(ac.pack_rgba4444(0, 0, 0, 0), 0x0000)
        self.assertEqual(ac.pack_rgba4444(255, 255, 255, 255), 0xFFFF)
        self.assertEqual(ac.pack_rgba4444(255, 0, 0, 0), 0xF000)
        self.assertEqual(ac.pack_rgba4444(0, 255, 0, 0), 0x0F00)
        self.assertEqual(ac.pack_rgba4444(0, 0, 255, 0), 0x00F0)
        self.assertEqual(ac.pack_rgba4444(0, 0, 0, 255), 0x000F)

    def test_encode_texture_little_endian_565(self):
        px = [(255, 0, 0), (0, 0, 255)]
        px = [(r, g, b, 255) for (r, g, b) in px]
        out = ac.encode_texture(px, ac.TEXFMT_RGBA565)
        # 0xF800 then 0x001F, little-endian
        self.assertEqual(out, bytes([0x00, 0xF8, 0x1F, 0x00]))

    def test_encode_texture_little_endian_4444(self):
        px = [(255, 0, 0, 255)]
        out = ac.encode_texture(px, ac.TEXFMT_RGBA4444)
        self.assertEqual(out, bytes([0x0F, 0xF0]))  # 0xF00F LE


class Rgba5551Tests(unittest.TestCase):
    """RGBA5551 round-trip and S0-spike known-value checks."""

    def test_pack_rgba5551_known_values(self):
        # All zeros -> 0x0000
        self.assertEqual(ac.pack_rgba5551(0, 0, 0, 0), 0x0000)
        # White opaque: R5=31 G5=31 B5=31 A1=1
        self.assertEqual(ac.pack_rgba5551(255, 255, 255, 255), 0xFFFF)
        # Red opaque: R5=31 G5=0 B5=0 A1=1
        self.assertEqual(ac.pack_rgba5551(255, 0, 0, 255), 0xF801)
        # Transparent black: 0x0000
        self.assertEqual(ac.pack_rgba5551(0, 0, 0, 0), 0x0000)
        # A1 threshold: a=127 -> 0, a=128 -> 1
        self.assertEqual(ac.pack_rgba5551(0, 0, 0, 127) & 1, 0)
        self.assertEqual(ac.pack_rgba5551(0, 0, 0, 128) & 1, 1)

    def test_unpack_rgba5551_be_s0_spike_known_value(self):
        # S0 spike: N64 big-endian 0x538b -> decoded -> RGB565 0x5385
        # R5=0b01010=10 G5=0b01110=14 B5=0b00010=2 A1=1
        # R8=(10*255+15)//31=83 G8=(14*255+15)//31=115 B8=(2*255+15)//31=21
        r8, g8, b8, a8 = ac.unpack_rgba5551_be(0x53, 0x8b)
        self.assertEqual(a8, 255)  # A1=1 -> 255
        # Verify these decode to the known RGB565 value (0x5385)
        rgb565 = ac.pack_rgb565(r8, g8, b8)
        self.assertEqual(rgb565, 0x5385,
                         'S0 spike: 0x538b BE RGBA5551 should decode to RGB565 0x5385, '
                         'got 0x%04x (R=%d G=%d B=%d)' % (rgb565, r8, g8, b8))

    def test_n64_rgba5551_be_to_le_blob_byte_swap(self):
        # Big-endian 0x538b -> LE bytes [0x8b, 0x53]
        raw = bytes([0x53, 0x8b, 0x21, 0x45])
        le = ac.n64_rgba5551_be_to_le_blob(raw)
        self.assertEqual(le, bytes([0x8b, 0x53, 0x45, 0x21]))

    def test_n64_rgba5551_be_to_le_blob_rejects_odd(self):
        with self.assertRaises(ValueError):
            ac.n64_rgba5551_be_to_le_blob(bytes([0x53]))

    def test_rgba5551_round_trip_pack_unpack(self):
        # Pack to LE, then unpack as BE (simulating read from LE storage)
        r, g, b, a = 200, 100, 50, 255
        v_le = ac.pack_rgba5551(r, g, b, a)
        lo = v_le & 0xFF
        hi = (v_le >> 8) & 0xFF
        # Our LE storage: lo first, hi second. Unpack with BE decoder (hi,lo swapped)
        r2, g2, b2, a2 = ac.unpack_rgba5551_be(hi, lo)
        # Within 5-bit quantization error
        self.assertLessEqual(abs(r2 - r), 9)
        self.assertLessEqual(abs(g2 - g), 9)
        self.assertLessEqual(abs(b2 - b), 9)
        self.assertEqual(a2, 255)


class I8I4Tests(unittest.TestCase):
    """I8 and I4 encoding round-trip tests."""

    def test_encode_i8_basic(self):
        vals = [0, 64, 128, 255]
        out = ac.encode_i8_blob(vals)
        self.assertEqual(out, bytes([0, 64, 128, 255]))

    def test_encode_i8_clamps(self):
        # Values > 255 should be masked to 8 bits
        out = ac.encode_i8_blob([256, 257])
        self.assertEqual(out, bytes([0, 1]))

    def test_encode_i4_basic(self):
        # 0xF0 -> hi=0xF, 0x00 -> lo=0x0 -> byte 0xF0
        # 0x80 -> hi=0x8, 0xFF -> lo=0xF -> byte 0x8F
        out = ac.encode_i4_blob([0xF0, 0x00, 0x80, 0xFF])
        self.assertEqual(out, bytes([0xF0, 0x8F]))

    def test_encode_i4_rejects_odd(self):
        with self.assertRaises(ValueError):
            ac.encode_i4_blob([0xFF])

    def test_encode_i4_roundtrip(self):
        vals = [0, 16, 32, 48, 64, 80, 96, 112]
        out = ac.encode_i4_blob(vals)
        self.assertEqual(len(out), 4)
        # Decode: each byte is two I4 nibbles
        decoded = []
        for b in out:
            decoded.append((b >> 4) * 16)
            decoded.append((b & 0xF) * 16)
        # Should round-trip with truncation (val >> 4 << 4 = val & 0xF0)
        for i in range(len(vals)):
            self.assertEqual(decoded[i], vals[i] & 0xF0)


class IA8IA4Tests(unittest.TestCase):
    """IA8 and IA4 encoding tests."""

    def test_encode_ia8_basic(self):
        # intensity=0xF0, alpha=0x0F -> hi=0xF, lo=0x0 -> byte 0xF0
        out = ac.encode_ia8_blob([(0xF0, 0x0F)])
        self.assertEqual(out, bytes([0xF0]))

    def test_encode_ia8_full_opaque(self):
        # intensity=255, alpha=255 -> 0xFF
        out = ac.encode_ia8_blob([(255, 255)])
        self.assertEqual(out, bytes([0xFF]))

    def test_encode_ia8_transparent(self):
        # intensity=255, alpha=0 -> 0xF0
        out = ac.encode_ia8_blob([(255, 0)])
        self.assertEqual(out, bytes([0xF0]))

    def test_encode_ia4_basic(self):
        # Texel 0: I=0xFF -> I3=7, A=0xFF -> A1=1 -> nib=0b1111=0xF
        # Texel 1: I=0x00 -> I3=0, A=0x00 -> A1=0 -> nib=0b0000=0x0
        out = ac.encode_ia4_blob([(255, 255), (0, 0)])
        self.assertEqual(out, bytes([0xF0]))

    def test_encode_ia4_rejects_odd(self):
        with self.assertRaises(ValueError):
            ac.encode_ia4_blob([(255, 255)])


class CI8CI4Tests(unittest.TestCase):
    """CI8, CI4, and TLUT encoding tests."""

    def test_encode_ci8_basic(self):
        out = ac.encode_ci8_blob([0, 127, 255])
        self.assertEqual(out, bytes([0, 127, 255]))

    def test_encode_ci4_basic(self):
        # Idx 0 hi, idx F lo -> byte 0x0F
        out = ac.encode_ci4_blob([0, 15])
        self.assertEqual(out, bytes([0x0F]))

    def test_encode_ci4_rejects_odd(self):
        with self.assertRaises(ValueError):
            ac.encode_ci4_blob([0])

    def test_encode_tlut_rgba5551_known(self):
        # One entry: R=255, G=0, B=0, A=255 -> pack_rgba5551(255,0,0,255)=0xF801
        # LE: bytes [0x01, 0xF8]
        out = ac.encode_tlut_rgba5551([(255, 0, 0, 255)])
        self.assertEqual(out, bytes([0x01, 0xF8]))

    def test_decode_tlut_from_n64_blob_known(self):
        # N64 BE RGBA5551 for white opaque: 0xFFFF -> hi=0xFF, lo=0xFF
        blob = bytes([0xFF, 0xFF])
        entries = ac.decode_tlut_from_n64_blob(blob, 1)
        r8, g8, b8, a8 = entries[0]
        self.assertEqual(a8, 255)
        self.assertEqual(r8, 255)
        self.assertEqual(g8, 255)
        self.assertEqual(b8, 255)

    def test_decode_tlut_from_n64_blob_s0_known(self):
        # 0x538b BE RGBA5551: validated from S0 spike
        blob = bytes([0x53, 0x8b])
        entries = ac.decode_tlut_from_n64_blob(blob, 1)
        r8, g8, b8, a8 = entries[0]
        self.assertEqual(a8, 255)
        rgb565 = ac.pack_rgb565(r8, g8, b8)
        self.assertEqual(rgb565, 0x5385)

    def test_tlut_round_trip(self):
        # 256-entry TLUT: encode N64 BE blob -> decode -> re-encode LE
        # Use two known entries
        blob = bytes([0x53, 0x8b, 0x21, 0x45] + [0xFF, 0xFF] * 254)
        entries = ac.decode_tlut_from_n64_blob(blob, 256)
        le_blob = ac.encode_tlut_rgba5551(entries)
        self.assertEqual(len(le_blob), 256 * 2)
        # First entry decoded then re-encoded
        v0 = struct.unpack_from('<H', le_blob, 0)[0]
        r8, g8, b8, a8 = ac.unpack_rgba5551_be(0x53, 0x8b)
        expected_v0 = ac.pack_rgba5551(r8, g8, b8, a8)
        self.assertEqual(v0, expected_v0)


class TerrainGridParserTests(unittest.TestCase):
    """Parser tests for N64 terrain_grid.c format."""

    SAMPLE_SRC = """\
Vtx g_terrain_grid[594] = {
  {{ {0, -308, 0}, 0, {32, 32}, {128, 128, 128, 255} }},
  {{ {512, -256, 0}, 0, {2080, 32}, {128, 128, 128, 255} }},
  {{ {256, -276, 0}, 0, {1056, 32}, {128, 128, 128, 255} }},
};
"""

    def test_parse_basic(self):
        verts = ac.parse_n64_terrain_grid_c(self.SAMPLE_SRC)
        self.assertEqual(len(verts), 3)
        self.assertEqual(verts[0]['pos'], (0, -308, 0))
        self.assertEqual(verts[0]['uv'], (32, 32))
        self.assertEqual(verts[0]['rgba'], (128, 128, 128, 255))
        self.assertEqual(verts[1]['pos'], (512, -256, 0))
        self.assertEqual(verts[1]['uv'], (2080, 32))

    def test_parse_negative_coords(self):
        src = '{{ {-100, -200, -300}, 0, {32, 32}, {0, 0, 0, 255} }}'
        verts = ac.parse_n64_terrain_grid_c(src)
        self.assertEqual(verts[0]['pos'], (-100, -200, -300))


class TerrainIndexTests(unittest.TestCase):
    """grid_index and tile_vert_indices correctness tests."""

    def test_grid_index_even_col(self):
        # col=0, row=0: base=0, even -> 0 + 0 = 0
        self.assertEqual(ac.grid_index(0, 0), 0)
        # col=2, row=0: even -> 0 + 1 = 1
        self.assertEqual(ac.grid_index(2, 0), 1)
        # col=16, row=0: even -> 0 + 8 = 8
        self.assertEqual(ac.grid_index(16, 0), 8)
        # col=0, row=1: base=17, even -> 17
        self.assertEqual(ac.grid_index(0, 1), 17)

    def test_grid_index_odd_col(self):
        # col=1, row=0: odd -> 0 + 9 + 0 = 9
        self.assertEqual(ac.grid_index(1, 0), 9)
        # col=3, row=0: odd -> 0 + 9 + 1 = 10
        self.assertEqual(ac.grid_index(3, 0), 10)
        # col=15, row=0: odd -> 0 + 9 + 7 = 16
        self.assertEqual(ac.grid_index(15, 0), 16)

    def test_tile_vert_indices_tile_0_0(self):
        # Tile (0,0): cols 0,1,2 x rows 0,1,2
        indices = ac.tile_vert_indices_terrain(0, 0)
        self.assertEqual(len(indices), 9)
        # [0,0] = grid_index(0,0) = 0
        self.assertEqual(indices[0], ac.grid_index(0, 0))
        # [0,1] = grid_index(1,0) = 9 (odd col)
        self.assertEqual(indices[1], ac.grid_index(1, 0))
        # [0,2] = grid_index(2,0) = 1
        self.assertEqual(indices[2], ac.grid_index(2, 0))
        # [1,0] = grid_index(0,1) = 17
        self.assertEqual(indices[3], ac.grid_index(0, 1))

    def test_tile_vert_indices_tile_1_0(self):
        # Tile (1,0): cols 2,3,4 x rows 0,1,2
        indices = ac.tile_vert_indices_terrain(1, 0)
        self.assertEqual(indices[0], ac.grid_index(2, 0))
        self.assertEqual(indices[1], ac.grid_index(3, 0))
        self.assertEqual(indices[2], ac.grid_index(4, 0))

    def test_tile_indices_pattern(self):
        # 8-triangle index pattern for a 3x3 block
        indices = ac.bake_terrain_tile_indices()
        self.assertEqual(len(indices), 24)  # 8 tris * 3 verts
        # Up-facing winding (NW,SW,NE / NE,SW,SE) for our Y-up CULL_BACK.
        # First quad: NW=0, NE=1, SW=3, SE=4
        self.assertEqual(indices[:6], [0, 3, 1, 1, 3, 4])
        # Second quad (qx=1): NW=1, NE=2, SW=4, SE=5
        self.assertEqual(indices[6:12], [1, 4, 2, 2, 4, 5])
        # Third quad (qy=1, qx=0): NW=3, NE=4, SW=6, SE=7
        self.assertEqual(indices[12:18], [3, 6, 4, 4, 6, 7])


class TerrainVertexBakeTests(unittest.TestCase):
    """Terrain vertex baking with pre-scale and UV computation."""

    SAMPLE_GRID_SRC = """\
Vtx g_terrain_grid[594] = {
  {{ {0, -308, 0}, 0, {32, 32}, {128, 128, 128, 255} }},
  {{ {512, -256, 0}, 0, {2080, 32}, {128, 128, 128, 255} }},
  {{ {1024, -244, 0}, 0, {4128, 32}, {128, 128, 128, 255} }},
  {{ {256, -276, 0}, 0, {1056, 32}, {128, 128, 128, 255} }},
  {{ {768, -248, 0}, 0, {3104, 32}, {128, 128, 128, 255} }},
  {{ {1280, -300, 0}, 0, {5152, 32}, {128, 128, 128, 255} }},
  {{ {0, -252, 256}, 0, {32, 1056}, {128, 128, 128, 255} }},
  {{ {512, -210, 256}, 0, {2080, 1056}, {128, 128, 128, 255} }},
  {{ {1024, -218, 256}, 0, {4128, 1056}, {128, 128, 128, 255} }},
  {{ {256, -214, 256}, 0, {1056, 1056}, {128, 128, 128, 255} }},
  {{ {768, -242, 256}, 0, {3104, 1056}, {128, 128, 128, 255} }},
  {{ {1280, -276, 256}, 0, {5152, 1056}, {128, 128, 128, 255} }},
  {{ {0, -240, 512}, 0, {32, 2080}, {128, 128, 128, 255} }},
  {{ {512, -212, 512}, 0, {2080, 2080}, {128, 128, 128, 255} }},
  {{ {1024, -218, 512}, 0, {4128, 2080}, {128, 128, 128, 255} }},
  {{ {256, -202, 512}, 0, {1056, 2080}, {128, 128, 128, 255} }},
  {{ {768, -218, 512}, 0, {3104, 2080}, {128, 128, 128, 255} }},
  {{ {1280, -252, 512}, 0, {5152, 2080}, {128, 128, 128, 255} }},
"""
    # Pad to 594 entries (terrain parser expects the pattern; we just need
    # indices 0..17 for tile (0,0) which uses rows 0-2, cols 0-2)

    def _make_grid_verts(self):
        # Build minimal synthetic grid for tile 0,0 test
        # tile (0,0) uses indices: grid_index(c,r) for c in {0,1,2}, r in {0,1,2}
        # grid_index(0,0)=0, grid_index(1,0)=9, grid_index(2,0)=1
        # grid_index(0,1)=17, grid_index(1,1)=26, grid_index(2,1)=18
        # grid_index(0,2)=34, grid_index(1,2)=43, grid_index(2,2)=35
        # Max index needed = 43. Build 50-entry list.
        dummy = {'pos': (0, 0, 0), 'uv': (32, 32), 'rgba': (128, 128, 128, 255)}
        verts = [dict(dummy) for _ in range(50)]
        # Set the 9 vertices for tile (0,0)
        # Row 0 even cols: idx 0=col0row0, 1=col2row0, 2=col4row0
        verts[0] = {'pos': (0, -308, 0), 'uv': (32, 32), 'rgba': (128, 128, 128, 255)}
        verts[9] = {'pos': (256, -276, 0), 'uv': (1056, 32), 'rgba': (128, 128, 128, 255)}
        verts[1] = {'pos': (512, -256, 0), 'uv': (2080, 32), 'rgba': (128, 128, 128, 255)}
        verts[17] = {'pos': (0, -252, 256), 'uv': (32, 1056), 'rgba': (128, 128, 128, 255)}
        verts[26] = {'pos': (256, -214, 256), 'uv': (1056, 1056), 'rgba': (128, 128, 128, 255)}
        verts[18] = {'pos': (512, -210, 256), 'uv': (2080, 1056), 'rgba': (128, 128, 128, 255)}
        verts[34] = {'pos': (0, -240, 512), 'uv': (32, 2080), 'rgba': (128, 128, 128, 255)}
        verts[43] = {'pos': (256, -202, 512), 'uv': (1056, 2080), 'rgba': (128, 128, 128, 255)}
        verts[35] = {'pos': (512, -212, 512), 'uv': (2080, 2080), 'rgba': (128, 128, 128, 255)}
        return verts

    def test_bake_tile_0_0_vertex_count(self):
        verts = self._make_grid_verts()
        tile_verts = ac.bake_terrain_tile_vertices(verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        self.assertEqual(len(tile_verts), 9)

    def test_bake_tile_0_0_pre_scale(self):
        # World x=512 -> pre-scaled pos x = round(512 * 0.005) = round(2.56) = 3
        verts = self._make_grid_verts()
        tile_verts = ac.bake_terrain_tile_vertices(verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        # vertex at grid idx 1 (col=2, row=0) has world x=512
        # In bake output: vertex idx 2 (cy=0, cx=2) of the tile
        pos2, uv2, rgba2 = tile_verts[2]
        self.assertEqual(pos2[0], ac.round_half_away(512 * ac.TERRAIN_PRE_SCALE))
        self.assertEqual(pos2[2], 0)

    def test_bake_tile_0_0_uv_gutter_offset(self):
        # Vertex cx=0, cy=0 (corner): UV should be (2.0, 1.0) in texel coords
        # (gutter offset: 2 cols left, 1 row top)
        verts = self._make_grid_verts()
        tile_verts = ac.bake_terrain_tile_vertices(verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        pos0, uv0, rgba0 = tile_verts[0]  # cx=0, cy=0
        self.assertAlmostEqual(uv0[0], 2.0, places=3)
        self.assertAlmostEqual(uv0[1], 1.0, places=3)

    def test_bake_tile_0_0_uv_center_col(self):
        # Vertex cx=1, cy=0: S_global=1056 -> s_tile=(1056/2 - 16)/32 + 2 = (528-16)/32+2 = 16+2=18
        verts = self._make_grid_verts()
        tile_verts = ac.bake_terrain_tile_vertices(verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        pos1, uv1, _ = tile_verts[1]  # cx=1, cy=0
        # Expected: (1056/2 - 16)/32 + 2 = (528-16)/32 + 2 = 512/32 + 2 = 16 + 2 = 18
        self.assertAlmostEqual(uv1[0], 18.0, places=3)

    def test_bake_tile_0_0_encodes_to_bytes(self):
        # Full encode round-trip: bake + encode_vertex_prelit, check sizes
        verts = self._make_grid_verts()
        tile_verts = ac.bake_terrain_tile_vertices(verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        blob = bytearray()
        for pos, uv, rgba in tile_verts:
            blob += ac.encode_vertex_prelit(pos, uv, rgba)
        self.assertEqual(len(blob), 9 * 14)

    def test_gutter_seam_check(self):
        # The gutter offset ensures that a bilinear tap at a tile edge samples the
        # gutter (replicated border) rather than content of a neighboring tile.
        # Concretely: the left-edge vertex (cx=0) has UV s=2.0 in the 36-wide tile.
        # The content starts at col 2. Bilinear at s=2.0 samples ONLY the tile's
        # own content edge and the replicated gutter col 1 — never col >35.
        verts = self._make_grid_verts()
        tile_verts = ac.bake_terrain_tile_vertices(verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        for cy in range(3):
            for cx in range(3):
                local = cy * 3 + cx
                _, uv, _ = tile_verts[local]
                s, t = uv
                # All UV s values must be within [0, 35] for the 36-wide tile
                self.assertGreaterEqual(s, 0.0,
                                        'UV s=%g out of lower bound at cx=%d cy=%d' % (s, cx, cy))
                self.assertLessEqual(s, 35.0,
                                     'UV s=%g out of upper bound at cx=%d cy=%d' % (s, cx, cy))
                self.assertGreaterEqual(t, 0.0,
                                        'UV t=%g out of lower bound at cx=%d cy=%d' % (t, cx, cy))
                self.assertLessEqual(t, 33.0,
                                     'UV t=%g out of upper bound at cx=%d cy=%d' % (t, cx, cy))


class TerrainAtlasTests(unittest.TestCase):
    """Single gutter'd atlas: pow2 dims, placement, and ALL-128 gutter-seam."""

    def test_atlas_dims_pow2(self):
        # 512-wide atlas: cell 36x35 -> 14 tiles/row, 10 rows, content H=350 ->
        # pow2 H=512.
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        self.assertEqual(tpr, 512 // ac.ATLAS_CELL_W)  # 14
        self.assertEqual(rows, (128 + tpr - 1) // tpr)  # 10
        # Atlas H must be pow2 and >= rows*ATLAS_CELL_H
        self.assertGreaterEqual(ah, rows * ac.ATLAS_CELL_H)
        self.assertEqual(ah & (ah - 1), 0, 'atlas H must be pow2, got %d' % ah)
        # 512 width is pow2
        self.assertEqual(512 & 511, 0)

    def test_atlas_cell_has_vertical_spacer(self):
        # The atlas cell is 1 row taller than the tile block (the replicated
        # bottom spacer that prevents vertical seam bleed).
        self.assertEqual(ac.ATLAS_CELL_W, ac.TILE_W)
        self.assertEqual(ac.ATLAS_CELL_H, ac.TILE_H + 1)

    def test_atlas_dims_rejects_non_pow2_width(self):
        with self.assertRaises(ValueError):
            ac.terrain_atlas_dims(128, 500)

    def test_atlas_tile_origin(self):
        # tile 0 -> (0,0); tile 13 (last in row 0) -> (13*36, 0); tile 14 ->
        # (0, ATLAS_CELL_H) since cell stride is 35 tall.
        self.assertEqual(ac.atlas_tile_origin(0, 14), (0, 0))
        self.assertEqual(ac.atlas_tile_origin(13, 14), (13 * ac.ATLAS_CELL_W, 0))
        self.assertEqual(ac.atlas_tile_origin(14, 14), (0, ac.ATLAS_CELL_H))
        # tile 127: col=127%14=1, row=127//14=9
        self.assertEqual(ac.atlas_tile_origin(127, 14),
                         (1 * ac.ATLAS_CELL_W, 9 * ac.ATLAS_CELL_H))

    def test_build_atlas_size_and_tile0_origin(self):
        # Synthetic: tile 0 first texel = 0x538b, rest zero.
        tex_values = [0x538b] + [0x0000] * (128 * 36 * 34 - 1)
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas = ac.build_terrain_atlas_rgba5551_le(tex_values, 128, 512, ah, tpr)
        self.assertEqual(len(atlas), 512 * ah * 2)
        # Tile 0 origin (0,0): first texel LE = lo=0x8b, hi=0x53
        self.assertEqual(atlas[0], 0x8b)
        self.assertEqual(atlas[1], 0x53)

    def test_build_atlas_spacer_replicates_bottom_row(self):
        # Tile 0 bottom block row (33) replicated to spacer row (34). Set tile 0
        # row 33 col 0 to a marker; verify atlas rows 33 AND 34 both carry it.
        texels_per_tile = 36 * 34
        tex_values = [0x0000] * (128 * texels_per_tile)
        tex_values[33 * 36 + 0] = 0x7E7E  # tile 0, row 33, col 0
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas = ac.build_terrain_atlas_rgba5551_le(tex_values, 128, 512, ah, tpr)
        off33 = (33 * 512 + 0) * 2
        off34 = (34 * 512 + 0) * 2
        self.assertEqual(atlas[off33], 0x7E)      # row 33 (block bottom)
        self.assertEqual(atlas[off33 + 1], 0x7E)
        self.assertEqual(atlas[off34], 0x7E)      # row 34 (spacer = copy of 33)
        self.assertEqual(atlas[off34 + 1], 0x7E)

    def test_build_atlas_tile1_placed_at_origin(self):
        # Tile 1 first texel = 0xABCD; it should land at atlas origin (36,0).
        texels_per_tile = 36 * 34
        tex_values = [0x0000] * (128 * texels_per_tile)
        tex_values[texels_per_tile] = 0xABCD  # tile 1 texel 0
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas = ac.build_terrain_atlas_rgba5551_le(tex_values, 128, 512, ah, tpr)
        ox, oy = ac.atlas_tile_origin(1, tpr)  # (36, 0)
        off = (oy * 512 + ox) * 2
        self.assertEqual(atlas[off], 0xCD)      # LE lo
        self.assertEqual(atlas[off + 1], 0xAB)  # LE hi

    def test_build_atlas_tile14_second_row(self):
        # Tile 14 first texel -> atlas origin (0, ATLAS_CELL_H) = (0, 35)
        texels_per_tile = 36 * 34
        tex_values = [0x0000] * (128 * texels_per_tile)
        tex_values[14 * texels_per_tile] = 0x1234
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas = ac.build_terrain_atlas_rgba5551_le(tex_values, 128, 512, ah, tpr)
        ox, oy = ac.atlas_tile_origin(14, tpr)  # (0, 35)
        self.assertEqual((ox, oy), (0, ac.ATLAS_CELL_H))
        off = (oy * 512 + ox) * 2
        self.assertEqual(atlas[off], 0x34)
        self.assertEqual(atlas[off + 1], 0x12)

    def _make_full_grid(self):
        # Real grid from N64 source if present, else synthetic minimal.
        if os.path.isdir(N64_TERRAIN_DIR):
            path = os.path.join(N64_TERRAIN_DIR, 'src', 'assets', 'terrain_grid.c')
            with open(path) as f:
                return ac.parse_n64_terrain_grid_c(f.read())
        return None

    def test_atlas_uv_all_128_subregions_seam_coverage(self):
        # COVERAGE GAP CLOSED: assert EVERY tile's 9 baked UVs land inside the
        # correct atlas sub-region [origin+gutter, origin+TILE-gutter], so a
        # bilinear tap at a content edge reads THIS tile's replicated gutter,
        # never a neighbor. Prior test covered only tile (0,0).
        grid_verts = self._make_full_grid()
        if grid_verts is None:
            self.skipTest('N64 terrain dir not found')
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas_verts = ac.bake_terrain_atlas_vertices(
            grid_verts, 512, ah, tpr, ac.TERRAIN_PRE_SCALE)
        self.assertEqual(len(atlas_verts), 128 * 9)
        for ty in range(ac.TERRAIN_TILES_Y):
            for tx in range(ac.TERRAIN_TILES_X):
                t = ty * ac.TERRAIN_TILES_X + tx
                ox, oy = ac.atlas_tile_origin(t, tpr)
                # Allowed content+edge window for this tile within the atlas:
                # in-tile UVs are [2,34] x [1,33]; add origin.
                s_lo = ox + ac.TILE_GUTTER_S
                s_hi = ox + (ac.TILE_W - ac.TILE_GUTTER_S)  # 34
                t_lo = oy + ac.TILE_GUTTER_T
                t_hi = oy + (ac.TILE_H - ac.TILE_GUTTER_T)  # 33
                for v in range(9):
                    pos, uv, rgba = atlas_verts[t * 9 + v]
                    s, tt = uv
                    self.assertGreaterEqual(
                        s, s_lo, 'tile %d v%d: s=%g < sub-region lo %g' % (t, v, s, s_lo))
                    self.assertLessEqual(
                        s, s_hi, 'tile %d v%d: s=%g > sub-region hi %g' % (t, v, s, s_hi))
                    self.assertGreaterEqual(
                        tt, t_lo, 'tile %d v%d: t=%g < sub-region lo %g' % (t, v, tt, t_lo))
                    self.assertLessEqual(
                        tt, t_hi, 'tile %d v%d: t=%g > sub-region hi %g' % (t, v, tt, t_hi))

    def test_atlas_bilinear_footprint_stays_in_cell_all_128(self):
        # THE seam check with teeth: a bilinear tap samples floor(uv) and
        # floor(uv)+1. For every tile's every vertex, that 2x2 footprint at the
        # MAX edge must stay within THIS tile's atlas cell (origin .. origin+
        # ATLAS_CELL_W/H), so it can never read a neighbor's content. The
        # horizontal 2-col gutter + vertical 1-row replicated spacer guarantee
        # this. Prior bake (no spacer) bled at the bottom edge (t=33 -> tap row
        # 34 = next stacked tile); this test would have caught it.
        grid_verts = self._make_full_grid()
        if grid_verts is None:
            self.skipTest('N64 terrain dir not found')
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas_verts = ac.bake_terrain_atlas_vertices(
            grid_verts, 512, ah, tpr, ac.TERRAIN_PRE_SCALE)
        for ty in range(ac.TERRAIN_TILES_Y):
            for tx in range(ac.TERRAIN_TILES_X):
                t = ty * ac.TERRAIN_TILES_X + tx
                ox, oy = ac.atlas_tile_origin(t, tpr)
                cell_s_max = ox + ac.ATLAS_CELL_W  # exclusive next-cell boundary
                cell_t_max = oy + ac.ATLAS_CELL_H
                for v in range(9):
                    pos, uv, rgba = atlas_verts[t * 9 + v]
                    s, tt = uv
                    # bilinear footprint upper texel = floor(uv)+1; for the seam
                    # to be safe it must be strictly inside the cell.
                    import math as _m
                    s_hi_tap = _m.floor(s) + 1
                    t_hi_tap = _m.floor(tt) + 1
                    self.assertLess(
                        s_hi_tap, cell_s_max,
                        'tile %d v%d: bilinear s-tap %d >= next cell %d (seam bleed)'
                        % (t, v, s_hi_tap, cell_s_max))
                    self.assertLess(
                        t_hi_tap, cell_t_max,
                        'tile %d v%d: bilinear t-tap %d >= next cell %d (seam bleed)'
                        % (t, v, t_hi_tap, cell_t_max))

    def test_atlas_uv_all_128_within_atlas_bounds(self):
        # Every baked UV must be strictly inside the pow2 atlas (CLAMP-safe).
        grid_verts = self._make_full_grid()
        if grid_verts is None:
            self.skipTest('N64 terrain dir not found')
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas_verts = ac.bake_terrain_atlas_vertices(
            grid_verts, 512, ah, tpr, ac.TERRAIN_PRE_SCALE)
        for i, (pos, uv, rgba) in enumerate(atlas_verts):
            s, t = uv
            self.assertGreaterEqual(s, 0.0, 'vert %d s=%g' % (i, s))
            self.assertLess(s, 512.0, 'vert %d s=%g >= atlas W' % (i, s))
            self.assertGreaterEqual(t, 0.0, 'vert %d t=%g' % (i, t))
            self.assertLess(t, float(ah), 'vert %d t=%g >= atlas H' % (i, t))

    def test_atlas_uv_no_subregion_overlap_between_tiles(self):
        # Two adjacent tiles must occupy disjoint atlas sub-regions; the gutter'd
        # block ensures their content windows never overlap. Verify tile 0's max
        # s (34) < tile 1's min s origin (36) -> a 2-texel gutter gap (cols 34,35
        # of tile 0 and cols 0,1 of tile 1 are the replicated borders).
        grid_verts = self._make_full_grid()
        if grid_verts is None:
            self.skipTest('N64 terrain dir not found')
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas_verts = ac.bake_terrain_atlas_vertices(
            grid_verts, 512, ah, tpr, ac.TERRAIN_PRE_SCALE)
        # tile 0 verts 0..8, tile 1 verts 9..17 (same row)
        t0_max_s = max(atlas_verts[v][1][0] for v in range(0, 9))
        t1_min_s = min(atlas_verts[v][1][0] for v in range(9, 18))
        # tile 0 content ends at <=34, tile 1 content starts at origin(36)+2=38
        self.assertLessEqual(t0_max_s, 34.0)
        self.assertGreaterEqual(t1_min_s, 38.0)
        self.assertLess(t0_max_s, t1_min_s)


class TerrainTexEncodeTests(unittest.TestCase):
    """Terrain texture encoding tests with known values."""

    def test_encode_tile_0_first_texel_known_s0_value(self):
        # terrain_tex.c tile 0 first texel = 0x538b (N64 big-endian RGBA5551)
        # LE bytes: [0x8b, 0x53]
        # Build synthetic tex_values: tile 0 = 36*34=1224 entries, first = 0x538b
        tex_values = [0x538b] + [0x0000] * (36 * 34 - 1)
        blob = ac.encode_terrain_tile_rgba5551_le(tex_values, 0, 36, 34)
        self.assertEqual(len(blob), 36 * 34 * 2)
        # First texel LE: lo=0x8b, hi=0x53
        self.assertEqual(blob[0], 0x8b)
        self.assertEqual(blob[1], 0x53)

    def test_encode_tile_multiple_values(self):
        # Second texel = 0x2145 -> LE: [0x45, 0x21]
        tex_values = [0x538b, 0x2145] + [0x0000] * (36 * 34 - 2)
        blob = ac.encode_terrain_tile_rgba5551_le(tex_values, 0, 36, 34)
        self.assertEqual(blob[2], 0x45)
        self.assertEqual(blob[3], 0x21)

    def test_extract_tile_pixels_s0_known(self):
        # Extract first tile's first pixel, verify it decodes to the S0-known RGBA
        tex_values = [0x538b] + [0x0000] * (36 * 34 - 1)
        pixels = ac.extract_tile_texture_pixels(tex_values, 0, 36, 34)
        r8, g8, b8, a8 = pixels[0]
        self.assertEqual(a8, 255)
        rgb565 = ac.pack_rgb565(r8, g8, b8)
        self.assertEqual(rgb565, 0x5385,
                         'First terrain texel should decode to 0x5385, got 0x%04x' % rgb565)


class TerrainHexArrayParserTests(unittest.TestCase):
    """parse_n64_hex_array_c tests."""

    def test_parse_u8_basic(self):
        src = '0x1f,0x15,0x05,\n0x0f,'
        vals = ac.parse_n64_hex_array_c(src, dtype='u8')
        self.assertEqual(vals, [0x1f, 0x15, 0x05, 0x0f])

    def test_parse_u16_basic(self):
        src = '0x538b, 0x2145,'
        vals = ac.parse_n64_hex_array_c(src, dtype='u16')
        self.assertEqual(vals, [0x538b, 0x2145])

    def test_parse_u8_rejects_overflow(self):
        with self.assertRaises(ValueError):
            ac.parse_n64_hex_array_c('0x100', dtype='u8')


class TerrainSceneryCylindersParserTests(unittest.TestCase):
    """parse_n64_scenery_cylinders_c tests."""

    SAMPLE = """\
const SceneryCylinder SCENERY_CYLINDERS[128] = {
  { 2294, -180, 3168, 8, 0, 149, 443 },
  { 1544, -368, 7828, 7, 0, 104, 349 },
  { -1, 0, 0, -1, 0, 0, 0 },
};
"""

    def test_parse_basic(self):
        entries = ac.parse_n64_scenery_cylinders_c(self.SAMPLE)
        self.assertEqual(len(entries), 3)
        self.assertEqual(entries[0]['cx'], 2294)
        self.assertEqual(entries[0]['y'], -180)
        self.assertEqual(entries[0]['sprite_id'], 8)
        self.assertEqual(entries[0]['radius'], 149)
        self.assertEqual(entries[0]['height'], 443)

    def test_parse_dead_entry(self):
        entries = ac.parse_n64_scenery_cylinders_c(self.SAMPLE)
        self.assertEqual(entries[2]['sprite_id'], -1)

    def test_scenery_pre_scale(self):
        entries = ac.parse_n64_scenery_cylinders_c(self.SAMPLE)
        e = entries[0]
        cx_scaled = ac.round_half_away(e['cx'] * ac.TERRAIN_PRE_SCALE)
        # 2294 * 0.005 = 11.47 -> 11
        self.assertEqual(cx_scaled, ac.round_half_away(2294 * 0.005))


class N64TerrainIntegrationTests(unittest.TestCase):
    """Integration tests against real N64 terrain source files.

    Skipped if ~/development/n64/terrain/ is not present.
    """

    def setUp(self):
        if not os.path.isdir(N64_TERRAIN_DIR):
            self.skipTest('N64 terrain dir not found: %s' % N64_TERRAIN_DIR)

    def _read_src(self, fname):
        path = os.path.join(N64_TERRAIN_DIR, 'src', 'assets', fname)
        with open(path) as f:
            return f.read()

    def test_terrain_grid_parse_count(self):
        src = self._read_src('terrain_grid.c')
        verts = ac.parse_n64_terrain_grid_c(src)
        self.assertEqual(len(verts), 594)
        # First live vertex known position
        self.assertEqual(verts[0]['pos'], (0, -308, 0))
        self.assertEqual(verts[0]['uv'], (32, 32))

    def test_terrain_tex_parse_count(self):
        src = self._read_src('terrain_tex.c')
        vals = ac.parse_n64_hex_array_c(src, dtype='u16')
        self.assertEqual(len(vals), 128 * 36 * 34)

    def test_terrain_tex_tile0_first_value_s0_known(self):
        src = self._read_src('terrain_tex.c')
        vals = ac.parse_n64_hex_array_c(src, dtype='u16')
        # First texel of tile 0 is 0x538b (from S0 spike and source inspection)
        self.assertEqual(vals[0], 0x538b)

    def test_terrain_tex_tile0_le_crc(self):
        src = self._read_src('terrain_tex.c')
        vals = ac.parse_n64_hex_array_c(src, dtype='u16')
        blob = ac.encode_terrain_tile_rgba5551_le(vals, 0, 36, 34)
        self.assertEqual(len(blob), 36 * 34 * 2)
        # First byte must be 0x8b (LE lo byte of 0x538b)
        self.assertEqual(blob[0], 0x8b)

    def test_real_atlas_512x512_pow2_and_s0_origin(self):
        # Bake the real atlas: dims pow2, tile 0 first texel at atlas (0,0) = 0x538b LE.
        src = self._read_src('terrain_tex.c')
        vals = ac.parse_n64_hex_array_c(src, dtype='u16')
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        self.assertEqual((512 & 511), 0)            # W pow2
        self.assertEqual(ah & (ah - 1), 0)          # H pow2
        atlas = ac.build_terrain_atlas_rgba5551_le(vals, 128, 512, ah, tpr)
        self.assertEqual(len(atlas), 512 * ah * 2)
        # Tile 0 origin (0,0): LE bytes of 0x538b
        self.assertEqual(atlas[0], 0x8b)
        self.assertEqual(atlas[1], 0x53)
        # Decode the atlas (0,0) texel; verify S0-spike RGB565 = 0x5385
        r8, g8, b8, a8 = ac.unpack_rgba5551_be(atlas[1], atlas[0])
        self.assertEqual(ac.pack_rgb565(r8, g8, b8), 0x5385)

    def test_real_atlas_tile_content_matches_source(self):
        # Sample tile 5's first texel: must equal source tile 5 first value, at
        # atlas origin (5*36, 0).
        src = self._read_src('terrain_tex.c')
        vals = ac.parse_n64_hex_array_c(src, dtype='u16')
        tpr, rows, ah = ac.terrain_atlas_dims(128, 512)
        atlas = ac.build_terrain_atlas_rgba5551_le(vals, 128, 512, ah, tpr)
        src_v = vals[5 * 36 * 34]  # tile 5 texel 0
        ox, oy = ac.atlas_tile_origin(5, tpr)
        off = (oy * 512 + ox) * 2
        self.assertEqual(atlas[off], src_v & 0xFF)
        self.assertEqual(atlas[off + 1], (src_v >> 8) & 0xFF)

    def test_terrain_detail_parse_count(self):
        src = self._read_src('terrain_detail.c')
        vals = ac.parse_n64_hex_array_c(src, dtype='u8')
        self.assertEqual(len(vals), 32 * 32)

    def test_panorama_tlut_parse_count(self):
        path = os.path.join(N64_TERRAIN_DIR, 'src', 'assets', 'panorama_tlut.inc.c')
        with open(path) as f:
            src = f.read()
        vals = ac.parse_n64_hex_array_c(src, dtype='u8')
        self.assertEqual(len(vals), 256 * 2)

    def test_panorama_img_parse_count(self):
        path = os.path.join(N64_TERRAIN_DIR, 'src', 'assets', 'panorama_img.inc.c')
        with open(path) as f:
            src = f.read()
        vals = ac.parse_n64_hex_array_c(src, dtype='u8')
        self.assertEqual(len(vals), 512 * 64)

    def test_tree0_parse_count(self):
        path = os.path.join(N64_TERRAIN_DIR, 'src', 'assets', 'tree0.inc.c')
        with open(path) as f:
            src = f.read()
        vals = ac.parse_n64_hex_array_c(src, dtype='u8')
        self.assertEqual(len(vals), 32 * 64 * 2)

    def test_scenery_parse_live_count(self):
        src = self._read_src('scenery_cylinders.c')
        entries = ac.parse_n64_scenery_cylinders_c(src)
        live = [e for e in entries if e['sprite_id'] >= 0]
        self.assertGreater(len(live), 0)
        self.assertLessEqual(len(live), 128)

    def test_tile0_gutter_seam(self):
        # Integration: bake real tile (0,0) and verify all UVs in [0,35] x [0,33]
        grid_src = self._read_src('terrain_grid.c')
        grid_verts = ac.parse_n64_terrain_grid_c(grid_src)
        tile_verts = ac.bake_terrain_tile_vertices(
            grid_verts, 0, 0, ac.TERRAIN_PRE_SCALE)
        for i, (pos, uv, rgba) in enumerate(tile_verts):
            s, t = uv
            self.assertGreaterEqual(s, 0.0, 'vertex %d: s=%g < 0' % (i, s))
            self.assertLessEqual(s, 35.0, 'vertex %d: s=%g > 35' % (i, s))
            self.assertGreaterEqual(t, 0.0, 'vertex %d: t=%g < 0' % (i, t))
            self.assertLessEqual(t, 33.0, 'vertex %d: t=%g > 33' % (i, t))


class UvFixedPointTests(unittest.TestCase):
    def test_s10_5_scale(self):
        self.assertEqual(ac.uv_to_s10_5(1.0), 32)
        self.assertEqual(ac.uv_to_s10_5(0.5), 16)
        self.assertEqual(ac.uv_to_s10_5(0.0), 0)
        self.assertEqual(ac.uv_to_s10_5(-1.0), -32)

    def test_s10_5_round_half_away(self):
        # 0.5 texel * 32 = 16 exactly; test a rounding case
        self.assertEqual(ac.uv_to_s10_5(1.0 / 32.0), 1)        # exactly 1
        self.assertEqual(ac.uv_to_s10_5(1.5 / 32.0), 2)        # 1.5 -> 2 away
        self.assertEqual(ac.uv_to_s10_5(-1.5 / 32.0), -2)

    def test_s10_5_clamps_int16(self):
        self.assertEqual(ac.uv_to_s10_5(100000.0), ac.INT16_MAX)
        self.assertEqual(ac.uv_to_s10_5(-100000.0), ac.INT16_MIN)


class VertexLayoutTests(unittest.TestCase):
    def test_prelit_vertex_exact_bytes(self):
        # pos=(1,-2,3) uv=(0.5,1.0)->(16,32) rgba=(0x11,0x22,0x33,0x44)
        out = ac.encode_vertex_prelit((1, -2, 3), (0.5, 1.0),
                                      (0x11, 0x22, 0x33, 0x44))
        self.assertEqual(len(out), 14)
        expect = struct.pack('<hhhhhBBBB', 1, -2, 3, 16, 32,
                             0x11, 0x22, 0x33, 0x44)
        self.assertEqual(out, expect)
        # field offsets per types.h: pos@0, uv@6, c@10
        self.assertEqual(struct.unpack_from('<h', out, 0)[0], 1)
        self.assertEqual(struct.unpack_from('<h', out, 6)[0], 16)
        self.assertEqual(out[10], 0x11)

    def test_lit_vertex_signed_normal(self):
        out = ac.encode_vertex_lit((0, 0, 0), (0.0, 0.0),
                                   (-128, 0, 127), 200)
        self.assertEqual(len(out), 14)
        self.assertEqual(struct.unpack_from('<b', out, 10)[0], -128)
        self.assertEqual(struct.unpack_from('<b', out, 11)[0], 0)
        self.assertEqual(struct.unpack_from('<b', out, 12)[0], 127)
        self.assertEqual(out[13], 200)

    def test_indices_le_u16(self):
        out = ac.encode_indices([0, 1, 2, 65535, 2, 1])
        self.assertEqual(out, struct.pack('<6H', 0, 1, 2, 65535, 2, 1))

    def test_indices_reject_non_triangle(self):
        with self.assertRaises(ValueError):
            ac.encode_indices([0, 1])


class CLayoutAgreementTest(unittest.TestCase):
    """Authority check: our emitted layout must equal the C compiler's struct
    Vtx layout (sizeof + field offsets). If types.h changes shape, this fails."""

    def test_c_struct_vtx_layout(self):
        cc = os.environ.get('CXX', 'g++')
        prog = (
            '#include <stdint.h>\n'
            '#include <stdio.h>\n'
            '#include <stddef.h>\n'
            '#include "rdr/types.h"\n'
            'int main(void){\n'
            '  printf("%zu %zu %zu %zu\\n", sizeof(struct Vtx),\n'
            '    offsetof(struct Vtx,pos), offsetof(struct Vtx,uv),\n'
            '    offsetof(struct Vtx,c));\n'
            '  return 0; }\n'
        )
        with tempfile.TemporaryDirectory() as td:
            src = os.path.join(td, 'probe.cc')
            exe = os.path.join(td, 'probe')
            with open(src, 'w') as f:
                f.write(prog)
            try:
                subprocess.run([cc, '-I', SRC_INCLUDE, src, '-o', exe],
                               check=True, capture_output=True)
            except (OSError, subprocess.CalledProcessError) as e:
                self.skipTest('C++ compiler unavailable: %r' % (e,))
            res = subprocess.run([exe], check=True, capture_output=True,
                                 text=True)
        size, off_pos, off_uv, off_c = (int(x) for x in res.stdout.split())
        # Our encoder produces 14-byte records with pos@0, uv@6, c@10.
        self.assertEqual(size, 14)
        self.assertEqual(off_pos, 0)
        self.assertEqual(off_uv, 6)
        self.assertEqual(off_c, 10)
        self.assertEqual(len(ac.encode_vertex_prelit(
            (0, 0, 0), (0.0, 0.0), (0, 0, 0, 0))), size)


class CliMeshTests(unittest.TestCase):
    """End-to-end OBJ -> Vtx/index via the CLI (no PIL needed)."""

    def setUp(self):
        sys.modules.pop('asset_tool', None)
        import asset_tool  # noqa: E402
        self.tool = asset_tool

    def test_obj_triangle_roundtrip(self):
        obj = ('v 0 0 0\nv 10 0 0\nv 0 10 0\n'
               'vt 0 0\nvt 1 0\nvt 0 1\n'
               'f 1/1 2/2 3/3\n')
        with tempfile.TemporaryDirectory() as td:
            op = os.path.join(td, 'tri.obj')
            vp = os.path.join(td, 'v.bin')
            ip = os.path.join(td, 'i.bin')
            with open(op, 'w') as f:
                f.write(obj)
            rc = self.tool.main(['mesh', op, vp, ip])
            self.assertEqual(rc, 0)
            with open(vp, 'rb') as fv:
                v = fv.read()
            with open(ip, 'rb') as fi:
                i = fi.read()
        self.assertEqual(len(v), 3 * 14)               # 3 verts * 14 B
        self.assertEqual(i, struct.pack('<3H', 0, 1, 2))
        # v0 pos/uv/rgba, v1 pos@stride
        self.assertEqual(struct.unpack_from('<3h', v, 0), (0, 0, 0))
        self.assertEqual(struct.unpack_from('<2h', v, 6), (0, 0))
        self.assertEqual(tuple(v[10:14]), (255, 255, 255, 255))
        self.assertEqual(struct.unpack_from('<3h', v, 14), (10, 0, 0))

    def test_obj_quad_fan_triangulates(self):
        obj = ('v 0 0 0\nv 1 0 0\nv 1 1 0\nv 0 1 0\n'
               'f 1 2 3 4\n')
        with tempfile.TemporaryDirectory() as td:
            op = os.path.join(td, 'q.obj')
            vp = os.path.join(td, 'v.bin')
            ip = os.path.join(td, 'i.bin')
            with open(op, 'w') as f:
                f.write(obj)
            self.tool.main(['mesh', op, vp, ip])
            with open(ip, 'rb') as fi:
                i = fi.read()
        # quad -> 2 tris fan: (0,1,2),(0,2,3)
        self.assertEqual(i, struct.pack('<6H', 0, 1, 2, 0, 2, 3))


if __name__ == '__main__':
    unittest.main()
