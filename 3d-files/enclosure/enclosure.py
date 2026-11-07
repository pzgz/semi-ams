#!/usr/bin/env python3
"""SEMI-AMS 外壳改版：PCB 保护壳扩大到盖住主体整面 + 盖板（cover）外侧加厚、螺丝孔做开口沉孔。

用法：  python3 enclosure.py [原厂目录] [--out 输出目录]
  原厂目录默认 ~/SynologyDrive/3D打印/20260819_semi-ams，要有：
    SEMI-AMS-main-body.stl   主体（只用来取外轮廓和孔位）
    SEMI-AMS-PCB保护壳.stl    原 PCB 保护壳（整块沿用，在它基础上往外扩）
    SEMI-AMS-cover.stl       原盖板（整块沿用，在它外侧加厚）
输出（都是打印姿态，外表面朝下）：
    semi-ams-pcb-shell_full.stl    扩大版 PCB 保护壳
    semi-ams-cover_thick.stl       加厚版盖板
    semi-ams-enclosure.3mf         Bambu Studio 工程：两件排在一块板上，带打印参数（P2S + PETG HF）

坐标约定（都是原厂 STL 的坐标）：
  主体：装 PCB 的那一面是 z=0，往外是 -z；盖板那一面是 z=22.5。
  PCB 壳：外表面 z=0，压 PCB 的口沿 z=9；XY 平移 SHELL_OFF 就是主体坐标。
  新 PCB 壳：沿用这个约定，外表面 z=0，压 PCB 的口沿 z=9，压主体的口沿 z=9+PCB 厚。
  盖板：外表面 z=0，朝内的面 z=3（凸台到 5.5）；装到主体上是 y→82.5-y、z→25.5-z。
"""
import argparse
import sys
from pathlib import Path

import numpy as np
import trimesh
from manifold3d import CrossSection, FillRule, Manifold, Mesh, OpType
from shapely.geometry import MultiPolygon, Point, Polygon, box
from shapely import set_precision
from shapely.ops import unary_union

HERE = Path(__file__).resolve().parent
DEFAULT_SRC = Path.home() / "SynologyDrive/3D打印/20260819_semi-ams"

P = dict(
    pcb_t=1.6,       # PCB 板厚（JLC 默认），决定压主体的口沿比压 PCB 的口沿低多少
    wall=1.0,        # 新加的侧壁厚（同原壳）
    clr=0.2,         # 新侧壁离 PCB 边的间隙
    notch=3.0,       # 新螺丝缺口：孔心到缺口内壁（M3 头 Ø5.5，单边留 0.25）
    tab_top=8.0,     # 压片顶面（离外表面），同原壳
    hole_d=3.2,      # 六个螺丝孔（原壳是 3.0）
    cover_add=3.0,   # 盖板外侧加厚
    cb_d=6.2,        # 盖板沉孔直径
    cb_depth=3.5,    # 盖板沉孔深（从新外表面算）
    cover_text=1.0,  # 盖板刻字深（同原来）
)

# 打印设置：写进 3MF 作为零件的单独设置（工程预设沿用 ../bracket/bambu_project_settings.json：
# P2S 0.4 喷嘴、0.20mm Standard、Bambu PETG HF、纹理 PEI 板）。两件都是外表面贴热床，薄壁多：
#   PCB 壳顶板 1 mm、侧壁 0.8~1.2 mm，盖板 6 mm 厚的框。
#   - arachne：0.8~1.2 mm 的墙用两条变线宽填满，不留缝；
#   - 底层 5 层 = 1 mm：PCB 壳整块顶板实心，刻字、按钮孔、LED 窗都在这 5 层里；
#   - 3 圈墙 + 25% 螺旋体：盖板的框和六个压片基本实心，螺丝压紧不塌；
#   - 不加裙边：外表面就是贴床那面，边缘要干净；大平面贴床，PETG 不会翘；
#   - 不加支撑：朝下的只有压片和沉孔顶的短桥接（约 6 mm，两三面有墙）。
PRINT_SETTINGS = {
    "wall_loops": "3",
    "wall_generator": "arachne",
    "sparse_infill_density": "25%",
    "sparse_infill_pattern": "gyroid",
    "top_shell_layers": "5",
    "bottom_shell_layers": "5",
    "seam_position": "back",
    "brim_type": "no_brim",
    "enable_support": "0",
}
PROJECT_SETTINGS = HERE.parent / "bracket" / "bambu_project_settings.json"
THREE_MF = "semi-ams-enclosure.3mf"

SHELL_OFF = (25.55, 30.75)                  # 原 PCB 壳 → 主体坐标（三个孔对上主体孔）
PCB_RECT = (25.55, 30.75, 56.848, 82.497)  # PCB 外形（Gerber 板框换算到主体坐标），圆角 R3
PCB_HOLES = [(28.50, 79.00), (53.36, 79.00), (53.36, 34.26)]          # ①②③ 压 PCB
BODY_HOLES = {(3.50, 79.00): "LT", (3.50, 14.84): "LB", (32.68, 3.50): "LB"}  # ④⑤⑥ 压主体，缺口朝哪两边开
COVER_HOLES = {(3.50, 3.50): "LB", (53.36, 3.50): "RB", (53.36, 48.24): "RT",
               (3.50, 67.66): "LT", (32.68, 79.00): "LT"}             # 盖板坐标


# ---------------------------------------------------------------- 工具
def sections(mesh, z):
    s = mesh.section(plane_origin=[0, 0, z], plane_normal=[0, 0, 1])
    p, _ = s.to_2D(to_2D=np.eye(4))
    return list(p.polygons_full)


def outer(mesh, z):
    return max(sections(mesh, z), key=lambda q: q.area)


def polys(g):
    if g.is_empty:
        return []
    if isinstance(g, Polygon):
        return [g]
    return [q for q in getattr(g, "geoms", []) if isinstance(q, Polygon) and not q.is_empty]


def clean(g, eps=1e-3):
    g = g.buffer(0)
    return unary_union([q for q in polys(g) if q.area > eps])


def extrude(g, z0, z1):
    """shapely 面 → manifold 柱体 [z0, z1]。"""
    rings = []
    for q in polys(clean(g)):
        ext = np.asarray(q.exterior.coords)[:-1]
        if Polygon(ext).exterior.is_ccw is False:
            ext = ext[::-1]
        rings.append(ext)
        for r in q.interiors:
            h = np.asarray(r.coords)[:-1]
            if Polygon(h).exterior.is_ccw:
                h = h[::-1]
            rings.append(h)
    if not rings:
        return Manifold()
    cs = CrossSection(rings, FillRule.Positive)
    return Manifold.extrude(cs, z1 - z0).translate((0, 0, z0))


def to_manifold(tm):
    tm = tm.copy()
    tm.merge_vertices()
    return Manifold(Mesh(vert_properties=np.asarray(tm.vertices, np.float32), tri_verts=np.asarray(tm.faces, np.uint32)))


def to_trimesh(m):
    m = m.simplify(1e-4)   # 收掉 0.1 µm 以下的碎边（原壳各层圆角的三角化不完全一样，叠起来会留下极小的台阶）
    m = Manifold.batch_boolean([q for q in m.decompose() if q.volume() > 1.0], OpType.Add)   # 去掉零体积碎片
    mm = m.to_mesh()
    return trimesh.Trimesh(np.asarray(mm.vert_properties)[:, :3], np.asarray(mm.tri_verts), process=False)


def rbox(x0, y0, x1, y1, r):
    return box(x0, y0, x1, y1).buffer(-r).buffer(r, resolution=16)


def notch_region(h, a, dirs, fillet=1.5):
    """螺丝缺口：孔周围 ±a 的圆角方块，朝 dirs 指定的边（L/R/T/B）一直开到外面。"""
    x, y = h
    parts = [rbox(x - a, y - a, x + a, y + a, fillet)]
    far = 200
    if "L" in dirs:
        parts.append(box(-far, y - a, x, y + a))
    if "R" in dirs:
        parts.append(box(x, y - a, far, y + a))
    if "T" in dirs:
        parts.append(box(x - a, y, x + a, far))
    if "B" in dirs:
        parts.append(box(x - a, -far, x + a, y))
    if "L" in dirs and "T" in dirs:
        parts.append(box(-far, y, x, far))
    if "L" in dirs and "B" in dirs:
        parts.append(box(-far, -far, x, y))
    if "R" in dirs and "T" in dirs:
        parts.append(box(x, y, far, far))
    if "R" in dirs and "B" in dirs:
        parts.append(box(x, -far, far, y))
    return unary_union(parts)


def through_holes(mesh, poly):
    """poly 内部的洞里，上下贯通（竖直射线打不到实体）的那些。"""
    out = []
    for r in poly.interiors:
        q = Polygon(r)
        pt = q.representative_point()
        loc, _, _ = mesh.ray.intersects_location([[pt.x, pt.y, mesh.bounds[0][2] - 1]], [[0, 0, 1]])
        if len(loc) == 0:
            out.append(q)
    return out


# ---------------------------------------------------------------- PCB 保护壳
def build_shell(body, shell):
    a, w, clr, pcb_t = P["notch"], P["wall"], P["clr"], P["pcb_t"]
    z_pcb = 9.0                    # 原壳压 PCB 的口沿
    z_body = z_pcb + pcb_t         # 新壳压主体的口沿
    F = Polygon(outer(body, 0.05).exterior)            # 主体这一面的外轮廓 = 新壳外轮廓
    pcb = rbox(*PCB_RECT, 3.0)

    sh = shell.copy()
    sh.apply_translation((*SHELL_OFF, 0))
    old_fp = Polygon(outer(sh, 0.75).exterior)         # 原壳顶板外轮廓（带三个角缺口）
    corners = polys(box(*old_fp.bounds).difference(old_fp))
    U = []
    for h in PCB_HOLES:
        U.append(max((q for q in corners if q.contains(Point(h))), key=lambda q: q.area))
    # 原轮廓的圆角在 ① ③ 缺口边上留下细尖角（现在那一边封起来了），尖角补成实体；缺口其余边界保持原样
    sx, sy = SHELL_OFF
    closed = [Point(sx, sy + 44.75), Point(sx + 24.3, sy)]   # ① 左下角、③ 左下角（原来朝外，现在封起来）
    cusps = unary_union([q for u in U for q in polys(u.difference(u.buffer(-0.6).buffer(0.6)))
                         if q.area > 0.05 and min(q.distance(c) for c in closed) < 1.5])
    U1, U2, U3 = [clean(q.difference(cusps)) for q in U]

    # ① 原来在壳的左上角，左边（PCB 外）现在要封起来；③ 原来在右下角，下边要封起来
    ya1 = U1.bounds[1]          # ① 缺口下内壁
    xa3 = U3.bounds[0]          # ③ 缺口左内壁
    K1 = U1.union(box(U1.bounds[0], U1.bounds[3] - 0.2, U1.bounds[2], 90)).intersection(F)
    K2 = unary_union([U2, box(U2.bounds[0], U2.bounds[3] - 0.2, 90, 90),
                      box(U2.bounds[2] - 0.2, U2.bounds[1], 90, 90)]).intersection(F)
    K3 = U3.union(box(U3.bounds[2] - 0.2, U3.bounds[1], 90, U3.bounds[3])).intersection(F)
    C = {h: notch_region(h, a, d).intersection(F) for h, d in BODY_HOLES.items()}
    K = unary_union([K1, K2, K3, *C.values()])

    # 原壳是纯 2.5D 的（只有这几个水平面：0 / 0.5 / 1 / 5.4 / 7.5 / 8 / 9），所以整个新壳按高度分层，
    # 每层在 2D 里把原壳截面和新加的部分合成一个面，再逐层拉伸叠起来。这样不会有两块实体贴面相交，
    # 导出的 STL 是干净的封闭实体（不会在切片软件里报"已修复 N 个错误"）。
    sx, sy = SHELL_OFF
    # 原壳里现在在壳里面的两面墙（左墙、下墙）去掉
    rm = unary_union([
        box(sx - 1, sy - 1, sx + 1.05, ya1 - w - 0.2),          # 左墙（停在 ① 缺口下内壁那道墙之前）
        box(sx - 1, sy - 1, xa3 - w - 0.3, sy + 1.05),          # 下墙（停在 ③ 缺口左内壁那道墙之前）
        box(sx - 1, sy - 1, sx + 3.2, sy + 3.2),                # 左下圆角
    ])
    # 去掉的 "M"（电机线出口，现在在壳里面了）：刻字填平
    txt = unary_union(sections(sh, 0.75)).difference(unary_union(sections(sh, 0.25)))
    M = unary_union([q.buffer(0.03) for q in polys(txt) if q.distance(Point(sx + 3.0, sy + 41.0)) < 1.0])

    # 顶板：整面减掉缺口，再减掉原壳顶板上的洞（刻字、按钮孔、LED 窗）
    def plate(z):
        old_holes = [q for q in polys(old_fp.difference(unary_union(sections(sh, z)))) if q.area > 0.01]
        return F.difference(K).difference(unary_union(old_holes))

    # 新侧壁（一圈外墙 + ④⑤⑥ 缺口围墙 + ①③ 封口墙）、④⑤⑥ 压片
    ring = F.difference(F.buffer(-w)).difference(pcb.buffer(clr)).difference(K)
    w456 = unary_union([q.buffer(w).difference(q) for q in C.values()]).intersection(F).difference(K)
    V1_up = box(sx - clr - w, ya1 - w, sx + 0.05, 90).intersection(F)      # PCB 面以上可以伸到 PCB 边
    V1_lo = box(sx - clr - w, ya1 - w, sx - clr, 90).intersection(F)       # PCB 厚度那一段离 PCB 边 clr
    H3_up = box(xa3 - w, sy - clr - w, 90, sy + 0.05).intersection(F)
    H3_lo = box(xa3 - w, sy - clr - w, 90, sy - clr).intersection(F)
    walls_up = clean(unary_union([ring, w456, V1_up, H3_up, cusps.intersection(F)]).difference(K))
    walls_lo = clean(unary_union([ring, w456, V1_lo, H3_lo]).difference(K).difference(pcb.buffer(clr)))
    walls_up = walls_up.buffer(-0.15, join_style=2).buffer(0.15, join_style=2)    # 去掉细碎的薄片
    walls_lo = walls_lo.buffer(-0.15, join_style=2).buffer(0.15, join_style=2)
    tabs = unary_union(list(C.values())).intersection(F)
    holes = unary_union([Point(h).buffer(P["hole_d"] / 2, resolution=32) for h in PCB_HOLES + list(BODY_HOLES)])

    def old(z):
        return unary_union(sections(sh, z))

    bands = [  # (z0, z1, 这一层的面)
        (0.0, 0.5, unary_union([plate(0.25), M])),
        (0.5, 1.0, plate(0.75)),
        (1.0, 5.4, unary_union([old(3.0).difference(rm), walls_up])),
        (5.4, 7.5, unary_union([old(6.5).difference(rm), walls_up])),
        (7.5, 8.0, unary_union([old(7.75).difference(rm), walls_up])),
        (8.0, z_pcb, unary_union([old(8.5).difference(rm), walls_up, tabs])),
        (z_pcb, z_body, unary_union([walls_lo, tabs])),
    ]
    m = Manifold()
    for z0, z1, g in bands:
        # 原壳截面在不同高度切到侧面三角形的斜边，会多出一些落在直线段上的点，各层不一样；
        # 先去掉共线点（simplify 1e-5，远小于圆弧的弦高），再对齐到 0.1 µm 网格，各层的同一面就完全一样
        g = set_precision(clean(g.difference(holes)).simplify(1e-5), 1e-4)
        m = m + extrude(g, z0, z1)
    info = dict(F=F, pcb=pcb, K=K, C=C, U=U, old_fp=old_fp, walls_up=walls_up, walls_lo=walls_lo,
                top=plate(0.75), z_body=z_body, ya1=ya1, xa3=xa3, cusps=cusps, bands=bands)
    return to_trimesh(m), info


# ---------------------------------------------------------------- 盖板
def build_cover(cover):
    add, r = P["cover_add"], P["cb_d"] / 2
    lo = unary_union(sections(cover, 0.25))
    hi = unary_union(sections(cover, 1.25))
    text = clean(hi.difference(lo), 0.01)              # 外表面的刻字（0~1 mm 深）
    base = outer(cover, 0.05)
    holes = through_holes(cover, base)                 # 窗口、螺丝孔：上下贯通
    foot = Polygon(base.exterior).difference(unary_union(holes))

    # 开口沉孔：Ø6.2 的圆，朝最近的两条边开口；离窗口/外边只剩薄片的地方一起切掉
    cb = unary_union([Point(h).buffer(r, resolution=48).union(notch_region(h, r, d, fillet=r - 0.01))
                      for h, d in COVER_HOLES.items()])
    solid = foot.difference(cb)
    thin = solid.difference(solid.buffer(-0.4).buffer(0.4))
    near = unary_union([Point(h).buffer(5.0) for h in COVER_HOLES])
    cb = unary_union([cb, thin.intersection(near)]).intersection(Polygon(base.exterior).buffer(1))

    m = to_manifold(cover)
    m = m + extrude(text, -0.01, P["cover_text"] + 0.01)          # 原刻字填平
    m = m + extrude(foot.buffer(-0.01), -add, 0.02)                # 外侧加厚（缩 0.01，侧面不和原件贴面）
    m = m - extrude(text, -add - 0.01, -add + P["cover_text"])     # 刻字挪到新外表面
    m = m - extrude(cb, -add - 0.01, -add + P["cb_depth"])         # 沉孔
    t = to_trimesh(m)
    t.apply_translation((0, 0, add))                               # 新外表面放到 z=0（打印朝下）
    return t, dict(foot=foot, cb=cb, text=text, holes=holes)


def write_3mf(path, parts):
    """parts: [(名字, 打印姿态的 trimesh)]，一块板，并排摆在中间。用 ../bracket 的 bambu3mf.py 和渲染器。"""
    import io
    sys.path.insert(0, str(HERE.parent / "bracket"))
    import bambu3mf
    from render import render
    gap = 12.0
    widths = [m.extents[0] for _, m in parts]
    x = -(sum(widths) + gap * (len(parts) - 1)) / 2
    items, shown = [], []
    for (name, mesh), w in zip(parts, widths):
        cx = x + w / 2
        items.append(dict(name=name, mesh=mesh, offset=(cx, 0.0), settings=PRINT_SETTINGS, plate=0))
        m = mesh.copy()
        m.apply_translation([cx - mesh.bounds[:, 0].mean(), -mesh.bounds[:, 1].mean(), -mesh.bounds[0][2]])
        shown.append((m, (0.18, 0.55, 0.78)))
        x += w + gap
    buf = io.BytesIO()
    render(shown, (0.55, 0.8, -0.6), size=512, transparent=True).save(buf, format="PNG")
    bambu3mf.write_project(path, items, PROJECT_SETTINGS, title="SEMI-AMS 外壳改版",
                           description="SEMI-AMS 整面 PCB 保护壳 + 加厚盖板，github.com/pzgz/semi-ams 3d-files/enclosure",
                           plate_names=["PCB 保护壳（整面） + 盖板（加厚）"], thumbnails=[buf.getvalue()])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", default=str(DEFAULT_SRC), help="原厂文件目录")
    ap.add_argument("--out", default=str(HERE), help="输出目录")
    args = ap.parse_args()
    src, out = Path(args.src).expanduser(), Path(args.out)
    body = trimesh.load(src / "SEMI-AMS-main-body.stl")
    shell = trimesh.load(src / "SEMI-AMS-PCB保护壳.stl")
    cover = trimesh.load(src / "SEMI-AMS-cover.stl")
    s, _ = build_shell(body, shell)
    c, _ = build_cover(cover)
    for name, t in [("semi-ams-pcb-shell_full.stl", s), ("semi-ams-cover_thick.stl", c)]:
        if not t.is_watertight:
            sys.exit(f"{name} 不是封闭实体")
        t.export(out / name)
        print(f"{name}: {np.round(t.extents, 2)} mm, {t.volume / 1000:.2f} cm³")
    write_3mf(out / THREE_MF, [("SEMI-AMS PCB 保护壳（整面）", s), ("SEMI-AMS 盖板（加厚）", c)])
    print(f"{THREE_MF}: 一块板，两个零件，P2S + PETG HF")


if __name__ == "__main__":
    main()
