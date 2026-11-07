"""简单的 numpy 光栅渲染，用来出预览图和 3MF 缩略图（不依赖 OpenGL / Blender）。"""
import math

import numpy as np


def look_basis(view_dir, up):
    f = np.asarray(view_dir, float); f /= np.linalg.norm(f)
    r = np.cross(f, up); r /= np.linalg.norm(r)
    return r, np.cross(r, f), f


def render(meshes, view_dir, up=(0, 0, 1), size=800, margin=0.06, transparent=False):
    """meshes: [(trimesh, (r, g, b))]，返回 PIL 图像。"""
    from PIL import Image
    r, u, f = look_basis(view_dir, up)
    allv = np.vstack([m.vertices for m, _ in meshes])
    P = np.stack([allv @ r, allv @ u], 1)
    lo, hi = P.min(0), P.max(0)
    span = (hi - lo).max() * (1 + 2 * margin); c = (lo + hi) / 2; scale = size / span
    zbuf = np.full((size, size), np.inf); img = np.ones((size, size, 3))
    light = -f + 0.6 * u + 0.3 * r; light /= np.linalg.norm(light)
    for m, col in meshes:
        tri = m.vertices[m.faces]; n = m.face_normals
        vis = (n @ f) < 0; tri, n = tri[vis], n[vis]
        sx = ((tri @ r) - c[0]) * scale + size / 2
        sy = size / 2 - ((tri @ u) - c[1]) * scale
        sz = tri @ f
        colv = np.array(col)[None, :] * (0.35 + 0.65 * np.clip(n @ light, 0, 1))[:, None]
        for i in range(len(tri)):
            x0, x1, x2 = sx[i]; y0, y1, y2 = sy[i]
            xa, xb = max(int(min(x0, x1, x2)), 0), min(int(math.ceil(max(x0, x1, x2))), size - 1)
            ya, yb = max(int(min(y0, y1, y2)), 0), min(int(math.ceil(max(y0, y1, y2))), size - 1)
            if xa > xb or ya > yb:
                continue
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-12:
                continue
            X, Y = np.meshgrid(np.arange(xa, xb + 1) + 0.5, np.arange(ya, yb + 1) + 0.5)
            a = ((y1 - y2) * (X - x2) + (x2 - x1) * (Y - y2)) / den
            b = ((y2 - y0) * (X - x2) + (x0 - x2) * (Y - y2)) / den
            inside = (a >= -1e-6) & (b >= -1e-6) & (1 - a - b >= -1e-6)
            if not inside.any():
                continue
            z = a * sz[i, 0] + b * sz[i, 1] + (1 - a - b) * sz[i, 2]
            sub = zbuf[ya:yb + 1, xa:xb + 1]
            upd = inside & (z < sub)
            sub[upd] = z[upd]
            img[ya:yb + 1, xa:xb + 1][upd] = colv[i]
    zb = zbuf.copy(); fin = np.isfinite(zb); zb[~fin] = zb[fin].max() + 50
    edge = (np.abs(np.diff(zb, axis=1, append=zb[:, -1:])) + np.abs(np.diff(zb, axis=0, append=zb[-1:, :]))) > 6.0 / scale
    img[edge] *= 0.35
    im = Image.fromarray((np.clip(img, 0, 1) * 255).astype(np.uint8))
    if transparent:
        alpha = Image.fromarray(np.where(fin | edge, 255, 0).astype(np.uint8))
        im.putalpha(alpha)
    return im
