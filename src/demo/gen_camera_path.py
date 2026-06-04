#!/usr/bin/env python3
"""gen_camera_path.py — OFFLINE bake of the D.7 scripted camera path to a
committed Q16.16 V*P table (src/demo/camera_path_gen.h).

WHY (Lead decision, honoring Q5/TW-05): the scripted camera's per-frame path
must be FLOAT-FREE on device so host==device fb_crc is BIT-IDENTICAL, not the
~3.4e-5 approximate a per-frame on-device float bake gives. The fix: bake every
scripted frame's V*P matrix offline to Q16.16 HERE and COMMIT the result; the
runtime per-frame path then just integer-indexes the committed table. Both host
and device read the SAME committed bytes -> bit-identical.

KEY INSIGHT: this Python bake and the old C float math do NOT need to agree
bit-for-bit. Only the COMMITTED table matters, and both host and device read it.
So we compute the visual path however is convenient and commit the result.

Convention (TW-05, locked): the renderer reads a column-major flat matrix as
out[row] = sum_k m[k*4+row]*v[k]. asset_convert.bake_mvp_q16_16(vp) bakes
flat[i*4+j] = q16(vp[i][j]) where vp is the N64 row-vector form
clip[j] = sum_i v[i]*vp[i][j]. Equating the two, vp[i][j] is exactly the
column-i / row-j element of the renderer's column-major matrix. So we build the
float V*P in the SAME column-major flat layout the C code used (m[col*4+row]),
reshape m[i*4+j] -> vp[i][j], and bake through the locked helper.

The build does NOT run this script — camera_path_gen.h is committed data (like
the other generated assets). Run it by hand and commit the header when the path
or its parameters change:

    python3 src/demo/gen_camera_path.py src/demo/camera_path_gen.h
"""

import math
import os
import sys

# READ-ONLY import of the TW-05-locked bake helpers from tools/asset_convert.py
# (already on main; we do NOT edit tools/). Add tools/ to the import path.
_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.normpath(os.path.join(_HERE, '..', '..', 'tools'))
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)
from asset_convert import bake_mvp_q16_16, to_q16_16  # noqa: E402

# ---- path parameters (MUST mirror src/demo/demo_scene.{h,cc}) --------------
# Keep these in sync with the header enums + the .cc keyframe math; the C side
# carries the schedule (frame count, interpolation), this side bakes the poses.
DEMO_SCENE_SCALE = 0.005       # S0 LOCKED pre-scale
CAM_KEYFRAMES = 4              # DEMO_CAM_KEYFRAMES (closed loop)
FRAMES_PER_SEG = 120           # DEMO_CAM_FRAMES_PER_SEG
FRAME_COUNT = CAM_KEYFRAMES * FRAMES_PER_SEG  # full loop -> committed table len

# N64 LOCKED initial eye, raw world coords (pre-scaled by DEMO_SCENE_SCALE).
N64_EYE_X = 3210.0
N64_EYE_Y = -3330.0
N64_EYE_Z = 7330.0

# Perspective (1:1 aspect, fill 240x240; near/far cover the pre-scaled field).
FOV_DEG = 50.0
ASPECT = 1.0
NEAR = 0.5
FAR = 80.0

# Screen (mirrors rdr/config.h RDR_SCREEN_W/H) — the viewport the baked horizon
# row is expressed in. The full-screen viewport maps NDC y -> screen row exactly
# as geom.cc does: row = (vp.y + h/2) - ndc_y*(h/2) = (H/2)*(1 - ndc_y), with
# ndc_y=+1 at the TOP. We replicate that map so the baked horizon_row lands on
# the SAME row the 3D terrain horizon projects to (T3 N4: seam-free sky meets
# terrain).
SCREEN_H = 240

# Panorama (T3a): the CI8 sky cylinder is PANORAMA_W texels around (== 360 deg),
# so a view azimuth of theta radians scrolls the cylinder by
# round(theta/2pi * PANORAMA_W). Mirrors tools/asset_tool.py _PANORAMA_W and the
# committed g_terrain_panorama (512x64).
PANORAMA_W = 512

FX16_16_ONE = 1 << 16


def fxd(v):
    """Float -> Q16.16 int, matching the C demo's fxd() domain (round nearest).
    Reused so the Python keyframe poses land on the same integer grid the C
    keyframes did (the visual path is preserved)."""
    return to_q16_16(v)


# ---- column-major Q16.16-free float matrix math (mirrors demo_scene.cc) -----
# We work in plain floats here (only the committed Q16.16 table is normative).
# Layout matches the C Mat4fx.m[]: m[col*4 + row].

def m_identity():
    m = [0.0] * 16
    m[0] = m[5] = m[10] = m[15] = 1.0
    return m


def m_perspective(fov_deg, aspect, n, f):
    m = [0.0] * 16
    fscale = 1.0 / math.tan(fov_deg * 0.5 * math.pi / 180.0)
    m[0] = fscale / aspect
    m[5] = fscale
    m[10] = f / (f - n)
    m[11] = 1.0           # w = z
    m[14] = -(f * n) / (f - n)
    return m


def m_lookat(ex, ey, ez, cx, cy, cz, ux, uy, uz):
    # forward f = normalize(center - eye)
    fx, fy, fz = cx - ex, cy - ey, cz - ez
    flen = math.sqrt(fx * fx + fy * fy + fz * fz)
    if flen > 0.0:
        fx, fy, fz = fx / flen, fy / flen, fz / flen
    # s = normalize(f x up)
    sx = fy * uz - fz * uy
    sy = fz * ux - fx * uz
    sz = fx * uy - fy * ux
    slen = math.sqrt(sx * sx + sy * sy + sz * sz)
    if slen > 0.0:
        sx, sy, sz = sx / slen, sy / slen, sz / slen
    # u = s x f
    vx = sy * fz - sz * fy
    vy = sz * fx - sx * fz
    vz = sx * fy - sy * fx
    m = m_identity()
    m[0] = sx
    m[4] = sy
    m[8] = sz
    m[1] = vx
    m[5] = vy
    m[9] = vz
    m[2] = fx
    m[6] = fy
    m[10] = fz
    m[12] = -(sx * ex + sy * ey + sz * ez)
    m[13] = -(vx * ex + vy * ey + vz * ez)
    m[14] = -(fx * ex + fy * ey + fz * ez)
    return m


def mat4_mul(a, b):
    # Column-major: out[col*4+row] = sum_k a[k*4+row]*b[col*4+k].
    out = [0.0] * 16
    for col in range(4):
        for row in range(4):
            acc = 0.0
            for k in range(4):
                acc += a[k * 4 + row] * b[col * 4 + k]
            out[col * 4 + row] = acc
    return out


# ---- scripted keyframe poses (mirrors keys_build_once in demo_scene.cc) -----
def build_keyframes():
    """Return CAM_KEYFRAMES (eye_q16[3], look_q16[3]) poses, Q16.16 ints, on the
    SAME integer grid the C keyframes used (poses preserved across the port)."""
    ex0 = N64_EYE_X * DEMO_SCENE_SCALE        # ~16.05
    ey0 = -(N64_EYE_Y * DEMO_SCENE_SCALE)     # ~16.65 (up; N64 frame is Y-down)
    ez0 = N64_EYE_Z * DEMO_SCENE_SCALE        # ~36.65
    radius = math.sqrt(ex0 * ex0 + ez0 * ez0)  # ~40 (XZ)
    height = ey0
    keys = []
    for k in range(CAM_KEYFRAMES):
        if k == 0:
            eye = (fxd(ex0), fxd(ey0), fxd(ez0))
        else:
            ang = math.atan2(ex0, ez0) + k * (math.pi / 6.0)
            rad = radius * 0.7 if k == 2 else radius
            hgt = height * 0.7 if k == 2 else height
            eye = (fxd(rad * math.sin(ang)), fxd(hgt), fxd(rad * math.cos(ang)))
        look = (0, 0, 0)
        keys.append((eye, look))
    return keys


def lerp_q16(a, b, num, den):
    """Integer Q16.16 lerp, mirroring lerp_q16 in demo_scene.cc (64-bit widen,
    truncating divide). Keeps the baked path visually identical to the runtime
    integer interpolation the schedule still drives."""
    delta = b - a
    return a + (delta * num) // den


def scripted_pose(frame):
    """Eye/look (Q16.16 ints) at a scripted frame, mirroring scripted_sample."""
    keys = build_keyframes()
    seg = (frame // FRAMES_PER_SEG) % CAM_KEYFRAMES
    nxt = (seg + 1) % CAM_KEYFRAMES
    num = frame % FRAMES_PER_SEG
    den = FRAMES_PER_SEG
    eye = tuple(lerp_q16(keys[seg][0][i], keys[nxt][0][i], num, den)
                for i in range(3))
    look = tuple(lerp_q16(keys[seg][1][i], keys[nxt][1][i], num, den)
                 for i in range(3))
    return eye, look


def bake_frame_mvp(frame):
    """Baked Q16.16 V*P (flat[16], column-major) for one scripted frame."""
    eye, look = scripted_pose(frame)
    ex, ey, ez = (eye[0] / FX16_16_ONE, eye[1] / FX16_16_ONE,
                  eye[2] / FX16_16_ONE)
    lx, ly, lz = (look[0] / FX16_16_ONE, look[1] / FX16_16_ONE,
                  look[2] / FX16_16_ONE)
    proj = m_perspective(FOV_DEG, ASPECT, NEAR, FAR)
    view = m_lookat(ex, ey, ez, lx, ly, lz, 0.0, 1.0, 0.0)
    vp_flat = mat4_mul(proj, view)  # column-major m[col*4+row]
    # Reshape m[i*4+j] -> vp[i][j] (column i, row j) for the locked bake helper.
    vp = [[vp_flat[i * 4 + j] for j in range(4)] for i in range(4)]
    return bake_mvp_q16_16(vp)  # flat[i*4+j] = q16(vp[i][j]); == m[i*4+j]


# ---- T3a sky table (panorama scroll + projected horizon row) ----------------
# Baked OFFLINE per scripted frame, like the V*P, so the per-frame DEVICE path
# stays float-free (host==device fb_crc). The runtime indexes
# g_scripted_sky[frame % SCRIPTED_FRAME_COUNT]. The free-fly path recomputes the
# same values in float at runtime (the sanctioned interactive exception).

def _project_ndc_y(vp_flat, px, py, pz):
    """NDC y of a world point through the column-major V*P (perspective divide).
    clip[row] = sum_k vp_flat[k*4+row] * P[k]; ndc_y = clip_y / clip_w."""
    cy = (vp_flat[0 * 4 + 1] * px + vp_flat[1 * 4 + 1] * py +
          vp_flat[2 * 4 + 1] * pz + vp_flat[3 * 4 + 1])
    cw = (vp_flat[0 * 4 + 3] * px + vp_flat[1 * 4 + 3] * py +
          vp_flat[2 * 4 + 3] * pz + vp_flat[3 * 4 + 3])
    return cy / cw if cw != 0.0 else 0.0


def sky_for_frame(frame):
    """(scroll_x, horizon_row) for one scripted frame, ints. scroll_x in
    [0, PANORAMA_W) from the view AZIMUTH; horizon_row in [0, SCREEN_H) from
    projecting a level (pitch-0) ray of the same azimuth at infinity through the
    SAME V*P (so the seam matches the 3D horizon)."""
    eye, look = scripted_pose(frame)
    ex, ey, ez = eye[0] / FX16_16_ONE, eye[1] / FX16_16_ONE, eye[2] / FX16_16_ONE
    lx, ly, lz = look[0] / FX16_16_ONE, look[1] / FX16_16_ONE, look[2] / FX16_16_ONE
    fx, fy, fz = lx - ex, ly - ey, lz - ez
    # Azimuth of the horizontal view heading -> panorama scroll (wrap mod W).
    az = math.atan2(fx, fz)                      # [-pi, pi]
    scroll = int(round(az / (2.0 * math.pi) * PANORAMA_W)) % PANORAMA_W
    # Level ray (pitch 0) of the same azimuth: zero the vertical component.
    hlen = math.sqrt(fx * fx + fz * fz)
    if hlen <= 0.0:
        return scroll, SCREEN_H // 2          # degenerate (looking straight up/down)
    lvl_x, lvl_z = fx / hlen, fz / hlen
    proj = m_perspective(FOV_DEG, ASPECT, NEAR, FAR)
    view = m_lookat(ex, ey, ez, lx, ly, lz, 0.0, 1.0, 0.0)
    vp_flat = mat4_mul(proj, view)
    big = 1.0e7                                # far point on the level ray ~ horizon
    ndc_y = _project_ndc_y(vp_flat, ex + big * lvl_x, ey, ez + big * lvl_z)
    row = int(round((SCREEN_H * 0.5) * (1.0 - ndc_y)))   # geom.cc viewport map
    if row < 0:
        row = 0
    if row > SCREEN_H - 1:
        row = SCREEN_H - 1
    return scroll, row


def emit_header(path):
    rows = [bake_frame_mvp(f) for f in range(FRAME_COUNT)]
    sky = [sky_for_frame(f) for f in range(FRAME_COUNT)]
    lines = []
    lines.append('// camera_path_gen.h — GENERATED by src/demo/gen_camera_path.py.')
    lines.append('// DO NOT EDIT. Committed Q16.16 V*P table for the D.7 scripted')
    lines.append('// camera path. Baked OFFLINE (TW-05 convention m[i*4+j]=VP[i][j]')
    lines.append('// via asset_convert.bake_mvp_q16_16) so the per-frame path is')
    lines.append('// float-free and host==device fb_crc is bit-identical. The build')
    lines.append('// does not run Python; this is committed data. Regenerate with:')
    lines.append('//   python3 src/demo/gen_camera_path.py src/demo/camera_path_gen.h')
    lines.append('// clang-format is disabled below (generated table).')
    lines.append('// clang-format off')
    lines.append('#ifndef DEMO_CAMERA_PATH_GEN_H')
    lines.append('#define DEMO_CAMERA_PATH_GEN_H')
    lines.append('')
    lines.append('#include "rdr/types.h"')
    lines.append('')
    lines.append('#define SCRIPTED_FRAME_COUNT %d' % FRAME_COUNT)
    lines.append('')
    lines.append('// One column-major Q16.16 V*P matrix per scripted frame; the')
    lines.append('// runtime indexes g_scripted_mvp[frame % SCRIPTED_FRAME_COUNT].')
    lines.append('// NOLINTBEGIN (generated table: suppress naming/magic-number lint)')
    lines.append('static const fx16_16 g_scripted_mvp[SCRIPTED_FRAME_COUNT][16] = {')
    for f in range(FRAME_COUNT):
        vals = ', '.join('%d' % v for v in rows[f])
        lines.append('    {%s},' % vals)
    lines.append('};')
    lines.append('// NOLINTEND')
    lines.append('')
    lines.append('// T3a sky table: per-frame {panorama scroll_x, horizon_row} for the')
    lines.append('// scrolling CI8 panorama. scroll_x in [0,%d) from the view azimuth;'
                 % PANORAMA_W)
    lines.append('// horizon_row in [0,%d) is where a level ray projects through this' % SCREEN_H)
    lines.append('// frame\'s V*P (geom.cc viewport map) -> the sky band anchors there so')
    lines.append('// it meets the 3D terrain horizon seam-free. Float-free at runtime')
    lines.append('// (committed); the free-fly path recomputes the same in float.')
    lines.append('// NOLINTBEGIN')
    lines.append('#define DEMO_PANORAMA_W %d' % PANORAMA_W)
    lines.append('static const int16_t g_scripted_sky[SCRIPTED_FRAME_COUNT][2] = {')
    for f in range(FRAME_COUNT):
        lines.append('    {%d, %d},' % (sky[f][0], sky[f][1]))
    lines.append('};')
    lines.append('// NOLINTEND')
    lines.append('')
    lines.append('#endif  // DEMO_CAMERA_PATH_GEN_H')
    lines.append('// clang-format on')
    lines.append('')
    text = '\n'.join(lines)
    with open(path, 'w') as fh:
        fh.write(text)
    return len(text), FRAME_COUNT


def main(argv):
    out = argv[1] if len(argv) > 1 else os.path.join(_HERE, 'camera_path_gen.h')
    size, n = emit_header(out)
    sys.stderr.write('wrote %s: %d frames, %d bytes\n' % (out, n, size))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
