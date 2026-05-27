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
