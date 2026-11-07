#!/usr/bin/env python3
"""SEMI-AMS 卡座支架（参数化生成）。

在原厂磁吸固定板的基础上改成 "[" 形卡座：背板对着主体侧面的三颗磁铁，
上臂的六边形凸起插进主体顶面的 B 孔（Ø8，调张紧螺丝的孔），
下臂是一根有弹性的悬臂，把主体往上顶住。

两种做法：
  一体式（sanja-riser）     背板后面直接是 Sanja 3D P2S 增高架的法式挂齿
  Multiconnect 卡座 + 适配器 背板后面开一条标准 Multiconnect v2 槽。
                             Multiboard、Skadis、openGrid、墙面用社区现成的 Multiconnect 连接件；
                             Sanja 增高架用这里的适配器（挂齿 + 两个连接头）

使用方向：进料接头（PC4-M6 气动接头）朝上，缓冲管从下面出。

依赖：  pip3 install numpy shapely trimesh manifold3d pillow
用法：  python3 bracket.py                       生成所有零件的 STL（打印姿态）和多打印板的 Bambu Studio 工程 3MF：
                                                 本目录有锁扣件就出 semi-ams-bracket_with-lock.3mf（锁扣件放在带 Sanja 挂齿的板上），
                                                 没有就出不带锁扣件的 semi-ams-bracket.3mf
        python3 bracket.py --add 别的零件.stl --suffix xxx
                                                 换成别的零件（按它自己的姿态）放到带 Sanja 挂齿的打印板上
        python3 bracket.py --assembly             另外输出装配姿态的 STL（和主体对位检查用）
        python3 bracket.py --set arm_t=1.8 ...    临时改参数

3MF 是 Bambu Studio 工程：P2S 0.4 喷嘴 + Bambu PETG HF + 0.20mm Standard（都是系统预设），
强度相关的设置（PRINT_SETTINGS）放在零件上，打开就能切片。

坐标（装配姿态）：
  X  垂直于磁铁面，指向主体（主体占 X = 0 ~ 57）；背板在 X < 0
  Y  主体厚度方向，Y = 0 是主体底板外侧，Y = 25.5 是盖板外侧
  Z  向上，Z = 0 是主体底面（缓冲管出口那一面），Z = 71.16 是主体顶面（B 孔所在面）
打印姿态：Y = 0 那一面贴热床（侧放），不需要支撑。
"""
import argparse
import io
import math
import sys
from pathlib import Path

import numpy as np
import trimesh
from manifold3d import CrossSection, FillRule, Manifold
from shapely.affinity import translate as sh_translate
from shapely.geometry import Point, Polygon, box
from shapely.ops import unary_union

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

# ---------------------------------------------------------------------------
# 打印设置（写进 3MF，作为零件的单独设置；工程预设见 bambu_project_settings.json）
# 侧放打印时所有受力方向（挂齿、上下臂弯曲）都在层内，强度主要靠墙：
# 5 圈墙每边约 2.2mm，背板 4.6、上臂 4、下臂 1.5、挂齿都基本是实心的；
# arachne 让 1.5mm 的下臂和挂齿尖用变线宽填满，不留缝；只有顶部锁扣块和挂齿那一段较厚，用 40% 螺旋体填充。
# 接缝放在 "back"（打印板后方）＝ 支架最下沿那条平边，避开下臂根部受拉的一侧。
# ---------------------------------------------------------------------------
PRINT_SETTINGS = {
    "wall_loops": "5",
    "wall_generator": "arachne",
    "sparse_infill_density": "40%",
    "sparse_infill_pattern": "gyroid",
    "top_shell_layers": "5",
    "bottom_shell_layers": "5",
    "seam_position": "back",
    "brim_type": "outer_only",
    "brim_width": "3",
    "brim_object_gap": "0.1",
    "enable_support": "0",
}
ADDED_PART_SETTINGS = dict(PRINT_SETTINGS, brim_type="no_brim")
PROJECT_SETTINGS = HERE / "bambu_project_settings.json"
LOCK_STL = HERE / "reinforced locking piece even thicker.stl"   # 2707930 的加厚锁扣件（MakerWorld 许可，不进 git）

# ---------------------------------------------------------------------------
# SEMI-AMS 主体实测尺寸（原厂 SEMI-AMS-main-body.stl + 盖板），换算到装配坐标
# ---------------------------------------------------------------------------
DEV = dict(
    h=71.16,          # 顶面到底面（主体 y 11.34 ~ 82.5）
    w=25.5,           # 厚度：主体 22.5 + 盖板 3.0
    b_x=14.5,         # B 孔中心到磁铁面
    b_y=17.05,        # B 孔中心到底板外侧
    b_d=8.0,          # B 孔直径（孔深 6，穿过顶面那层壁）
    top_flat_x=26.0,  # 顶面平整到 X=26，再往外是进料接头那一块凸台
    bot_flat_x=34.5,  # 底面平整到 X=34.5，再往外是缓冲管出口
    # 磁铁孔 Ø6.2 × 3 深，(Y, Z)
    magnets=[(17.5, 60.12), (5.0, 60.12), (11.25, 47.5)],
)

# ---------------------------------------------------------------------------
# 可调参数
# ---------------------------------------------------------------------------
DEFAULTS = dict(
    width=25.5,        # 支架宽度（Y），默认和主体一样厚
    wall=4.6,          # 背板厚度（磁铁槽 3.1 + 底 1.5）
    mag_d=6.2,         # 磁铁槽直径（6×3 磁铁，压入或点胶）
    mag_depth=3.1,     # 磁铁槽深度（磁铁比背板表面低 0.1）
    top_t=4.0,         # 上臂厚度（不需要弹性）
    top_len=23.0,      # 上臂长度（顶面平整区到 X=26，留 3mm）
    top_fillet=1.5,    # 上臂和背板的内圆角（主体这个角是 R2）
    boss_ac=7.4,       # 六边形凸起对角距离（B 孔 Ø8，单边留 0.3）
    boss_h=2.2,        # 六边形凸起高度（B 孔深 6）
    boss_chamfer=0.5,  # 凸起端部倒角
    arm_t=1.5,         # 下臂厚度（弹性）
    arm_len=20.0,      # 下臂长度
    arm_gap=0.8,       # 下臂上表面和主体底面的间隙（只靠末端的凸点接触）
    pad_len=5.0,       # 末端凸点长度
    preload=0.5,       # 凸点和主体底面的过盈量，装好后下臂被压下这么多，把主体往上顶
    arm_fillet=2.0,    # 下臂根部内圆角（主体这个角是 R3）
    keyhole_stop=1.0,  # 挂钩锁扣孔底部的挡板厚度（在 Y=0 一侧）
    # Multiconnect 版
    wall_mc=8.3,       # 背板厚度：磁铁槽 3.1 + 中间 1.05 + Multiconnect 槽 4.15
    mc_detent=1,       # 1 = 槽顶保留 v2 卡点（0.4mm 收窄），0 = QuickRelease（没有卡点）
    mc_head=10.0,      # 适配器连接头半宽（标准连接头 R10，槽 R10.15）
    mc_spacing=25.0,   # 适配器上两个连接头的中心距
    adapter_t=4.0,     # 适配器板厚
)

# Multiconnect v2 尺寸（David D 公布的标准；和 QuackWorks multiconnectSlotDesign.scad 一致）
# 槽（母）：从背板背面往里量 s，半宽 l
MC_SLOT = dict(rn=7.65, rw=10.15, s_neck=0.438, s_cham=2.938, depth=4.15)
# 连接头（公）：从板面往外量 h，半宽 l；标准头是 R10、直段 1.0、45° 倒角 2.5、颈 R7.5 长 0.5，总高 4.0
MC_HEAD = dict(rn=7.5, h_neck=0.5, h_cham=3.0, h_top=4.0)
MC_TOP_MARGIN = 2.85   # 槽顶端离卡座顶面（标准：头中心在顶面下 13，槽顶 = 中心 + 10.15）
MC_RIDGE = (0.4, 8.0)  # v2 卡点：槽顶下方两侧各收窄 0.4，8mm 内渐变回去


# ---------------------------------------------------------------------------
# 2D / 3D 工具
# ---------------------------------------------------------------------------
def _contours(geom):
    polys = getattr(geom, "geoms", [geom])
    out = []
    for p in polys:
        if p.is_empty:
            continue
        out.append(np.asarray(p.exterior.coords)[:-1])
        out.extend(np.asarray(r.coords)[:-1] for r in p.interiors)
    return out


def extrude_xz(geom, y0, y1):
    """把 X-Z 平面里的 shapely 图形沿 Y 从 y0 拉伸到 y1。"""
    cs = CrossSection(_contours(geom), FillRule.EvenOdd)
    m = Manifold.extrude(cs, y1 - y0)            # (x, z_profile, 0..h)
    m = m.rotate([90, 0, 0])                    # (x, y, z) -> (x, -z, y)
    return m.translate([0, y1, 0])


def fillet_patch(cx, cz, sx, sz, r):
    """凹角圆角：角点 (cx, cz)，两条边分别沿 +sx·X 和 +sz·Z 方向离开角点。"""
    sq = box(min(cx, cx + sx * r), min(cz, cz + sz * r), max(cx, cx + sx * r), max(cz, cz + sz * r))
    return sq.difference(Point(cx + sx * r, cz + sz * r).buffer(r, 64))


def round_corners(poly, r):
    return poly.buffer(-r, 32).buffer(r, 32)


def arc_pad(x0, x1, z_base, z_top, n=40):
    """圆弧形凸点：弦从 x0 到 x1（在 z_base），顶点在 z_top。"""
    c, h = x1 - x0, z_top - z_base
    R = (c * c / 4 + h * h) / (2 * h)
    xc, zc = (x0 + x1) / 2, z_top - R
    half = math.asin(c / 2 / R)
    ang = np.linspace(math.pi / 2 + half, math.pi / 2 - half, n)
    pts = [(xc + R * math.cos(a), zc + R * math.sin(a)) for a in ang]
    return Polygon([(x0, z_base - 0.3)] + pts + [(x1, z_base - 0.3)])


def hex_xy(ac):
    """对角距离 ac 的正六边形（X-Y 平面），两个顶点朝 ±X、平边朝 ±Y。
    侧放打印时（Y 朝上）底下是一条 3.7mm 的平边，两侧斜边 60°，不需要支撑。"""
    r = ac / 2
    return CrossSection([[(r * math.cos(math.radians(60 * k)), r * math.sin(math.radians(60 * k))) for k in range(6)]])


def to_trimesh(m):
    mesh = m.to_mesh()
    return trimesh.Trimesh(vertices=np.asarray(mesh.vert_properties)[:, :3], faces=np.asarray(mesh.tri_verts), process=True)


# ---------------------------------------------------------------------------
# 卡座本体（所有挂载版本共用）
# ---------------------------------------------------------------------------
def cradle(P, wall=None):
    wall = P["wall"] if wall is None else wall
    H = DEV["h"]
    z_top = H + P["top_t"]
    z_arm_top = -P["arm_gap"]
    z_bot = z_arm_top - P["arm_t"]
    W = P["width"]

    plate = box(-wall, z_bot, 0, z_top)
    top_arm = round_corners(box(-wall, H, P["top_len"], z_top), 1.0)
    bottom_arm = round_corners(box(-wall, z_bot, P["arm_len"], z_arm_top), 0.6)
    pad = arc_pad(P["arm_len"] - 0.3 - P["pad_len"], P["arm_len"] - 0.3, z_arm_top, P["preload"])
    prof = unary_union([
        plate, top_arm, bottom_arm, pad,
        fillet_patch(0, H, 1, -1, P["top_fillet"]),            # 上臂内角
        fillet_patch(0, z_arm_top, 1, 1, P["arm_fillet"]),      # 下臂根部
    ])
    body = extrude_xz(prof, 0, W)

    # 六边形凸起（从上臂下表面往下伸进 B 孔）
    r = P["boss_ac"] / 2
    s = (r - P["boss_chamfer"]) / r
    hexcs = hex_xy(P["boss_ac"])
    straight = Manifold.extrude(hexcs, P["boss_h"] - P["boss_chamfer"] + 0.5)
    straight = straight.translate([0, 0, H - P["boss_h"] + P["boss_chamfer"]])
    tip = Manifold.extrude(hexcs, P["boss_chamfer"], scale_top=(s, s)).mirror([0, 0, 1])
    tip = tip.translate([0, 0, H - P["boss_h"] + P["boss_chamfer"]])
    boss = (straight + tip).translate([DEV["b_x"], DEV["b_y"], 0])
    body = body + boss

    # 磁铁槽（开口朝主体）
    for (my, mz) in DEV["magnets"]:
        cyl = Manifold.cylinder(P["mag_depth"] + 0.2, P["mag_d"] / 2, circular_segments=72)
        cyl = cyl.rotate([0, 90, 0]).translate([-P["mag_depth"], my, mz])
        body = body - cyl

    geo = dict(z_top=z_top, z_bot=z_bot, x_back=-wall, W=W)
    return body, geo


# ---------------------------------------------------------------------------
# 挂载方式：在背板后面（X < -wall）加东西。返回 (要加的, 要减的)
# ---------------------------------------------------------------------------
def mount_sanja_riser(P, geo):
    """挂在 Sanja 3D P2S 增高架（AMS Flipper Riser）的侧边导轨上。

    接口按 MakerWorld 2707930 挂钩实测的尺寸重新画（没有复制它的网格）：
    顶部一个锁扣孔（可以插原模型的锁扣件），往下两个向下斜的法式挂齿，插进增高架侧面的槽里。
    剖面坐标 (u, v)：u 从支架顶端往下量，v 从背板背面往增高架方向量。
    """
    xb, zt, W = geo["x_back"], geo["z_top"], geo["W"]

    def uv(points):
        return Polygon([(xb - v, zt - u) for (u, v) in points])

    # 顶部锁扣块：u 0~8.38，伸出 6.11；外下角 R1.17，内侧和底条之间 R1.98
    block = Polygon([(xb - v, zt - u) for (u, v) in [(0, 0), (0, 6.11), (8.38, 6.11), (8.38, 0)]])
    outer = (0, 6.11)
    oc = Point(xb - (outer[1] - 1.17), zt - (outer[0] + 1.17))
    corner_cut = box(xb - 6.11, zt - 1.17, xb - (6.11 - 1.17), zt).difference(oc.buffer(1.17, 48))
    block = block.difference(corner_cut)
    base = uv([(8.0, 0), (8.0, 0.76), (35.54, 0.76), (35.54, 0)])
    inner_fillet = box(xb - 2.75, zt - 10.37, xb - 0.76, zt - 8.38).difference(
        Point(xb - 2.75, zt - 10.37).buffer(1.99, 48))
    tooth_a = uv([(16.99, 0), (16.99, 0.76), (21.27, 3.97), (29.11, 3.97), (24.99, 0.76), (24.99, 0)])
    tooth_b = uv([(28.0, 0), (28.0, 0.76), (30.68, 2.10), (31.81, 2.57), (32.61, 2.82),
                  (36.17, 3.95), (36.92, 3.94), (35.54, 2.80), (35.54, 0)])
    prof = unary_union([block, base, inner_fillet, tooth_a, tooth_b])
    add = extrude_xz(prof, 0, W)

    # 锁扣孔：Ø4.68 圆孔 + 2.29 宽开口，朝增高架方向开口；Y=0 一侧留挡板，从 Y=W 一侧插入锁扣
    hole = unary_union([
        Point(xb - 2.50, zt - 4.04).buffer(2.34, 64),
        box(xb - 6.11 - 1, zt - 5.22, xb - 2.50, zt - 2.93),
    ])
    cut = extrude_xz(hole, P["keyhole_stop"], W + 1)
    return add, cut


def extrude_yz(cs, x0, x1):
    """把 Y-Z 平面里的 CrossSection（坐标 (y, z)）沿 X 从 x0 拉伸到 x1。"""
    m = Manifold.extrude(cs, x1 - x0)                       # (y, z, 0..h)
    m = m.transform([[0, 0, 1, x0], [1, 0, 0, 0], [0, 1, 0, 0]])
    return m


def extrude_z(cs, z0, z1):
    """把 X-Y 平面里的 CrossSection 沿 Z 从 z0 拉伸到 z1。"""
    return Manifold.extrude(cs, z1 - z0).translate([0, 0, z0])


def mc_slot_geo(P, geo):
    c = geo["W"] / 2
    z_end = geo["z_top"] - MC_TOP_MARGIN
    return c, z_end, z_end - MC_SLOT["rw"]        # 槽中心线 Y、槽顶、标准圆头中心（v2 卡点从这里往下）


def mount_multiconnect(P, geo):
    """背板后面开一条竖直的 Multiconnect v2 槽，下端开口，上端封死。

    和标准件的区别只有两点，都是为了侧放打印不用支撑：槽顶是平的（标准是半圆），没有 on-ramp。
    标准圆连接头照样能插进来、顶到槽顶、过卡点。
    """
    xb, W = geo["x_back"], geo["W"]
    c, z_end, z_r = mc_slot_geo(P, geo)
    S = MC_SLOT
    sec = CrossSection([[(xb - 1, c - S["rn"]), (xb + S["s_neck"], c - S["rn"]), (xb + S["s_cham"], c - S["rw"]),
                         (xb + S["depth"], c - S["rw"]), (xb + S["depth"], c + S["rw"]), (xb + S["s_cham"], c + S["rw"]),
                         (xb + S["s_neck"], c + S["rn"]), (xb - 1, c + S["rn"])]])
    tool = extrude_z(sec, geo["z_bot"] - 1, z_end)
    if P["mc_detent"]:
        t, L = MC_RIDGE
        for sgn in (1, -1):
            tri = CrossSection([[(c + sgn * S["rw"], z_r), (c + sgn * (S["rw"] - t), z_r), (c + sgn * S["rw"], z_r - L)]])
            if sgn < 0:
                tri = CrossSection([[(c - S["rw"], z_r), (c - S["rw"], z_r - L), (c - (S["rw"] - t), z_r)]])
            tool = tool - extrude_yz(tri, xb - 1.5, xb + S["depth"] + 0.5)
    return None, tool


MOUNTS = {
    "sanja-riser": dict(wall="wall", fn=mount_sanja_riser),        # 一体式
    "multiconnect": dict(wall="wall_mc", fn=mount_multiconnect),   # 通用卡座
}


def build(mount, P):
    spec = MOUNTS[mount]
    body, geo = cradle(P, P[spec["wall"]])
    add, cut = spec["fn"](P, geo)
    part = body
    if add is not None:
        part = part + add
    if cut is not None:
        part = part - cut
    if part.status().name != "NoError":
        raise RuntimeError(f"manifold error: {part.status()}")
    return part, geo


# ---------------------------------------------------------------------------
# 适配器（配 Multiconnect 卡座）：背面适配不同场景，正面两个连接头插进卡座的槽
# ---------------------------------------------------------------------------
def mc_head(P, x_face, c, z_end):
    """侧放可打印的 Multiconnect 连接头，顶面在 z_end。

    截面（X-Y）照标准连接头：颈 R7.5、45° 倒角到 R10，头部外角再切 45°（侧放时下面不悬空）；
    竖直方向（Y-Z）是八角形：R10 的直段在 v2 卡点上方，装到位时不受挤压，拔出时要挤过卡点。
    """
    H, hr = MC_HEAD, P["mc_head"]
    z_r = z_end - MC_SLOT["rw"]
    sec = CrossSection([[(x_face - 1, c - H["rn"]), (x_face + H["h_neck"], c - H["rn"]), (x_face + H["h_cham"], c - hr),
                         (x_face + H["h_top"], c - hr + 1), (x_face + H["h_top"], c + hr - 1), (x_face + H["h_cham"], c + hr),
                         (x_face + H["h_neck"], c + H["rn"]), (x_face - 1, c + H["rn"])]])
    z_lo = z_r + 0.25 - (hr - H["rn"])
    outline = CrossSection([[(c - H["rn"], z_end), (c - hr, z_end - (hr - H["rn"])), (c - hr, z_r + 0.25), (c - H["rn"], z_lo),
                             (c + H["rn"], z_lo), (c + hr, z_r + 0.25), (c + hr, z_end - (hr - H["rn"])), (c + H["rn"], z_end)]])
    return extrude_z(sec, z_lo - 1, z_end + 1) ^ extrude_yz(outline, x_face - 1, x_face + H["h_top"] + 1)


def adapter_sanja_riser(P):
    """Sanja 3D P2S 增高架适配器：背面和一体式完全一样的挂齿 + 锁扣孔，正面两个连接头。"""
    W = P["width"]
    z_top = DEV["h"] + P["top_t"]
    x_face = -P["wall_mc"]                   # 正面 = 卡座背面
    x_back = x_face - P["adapter_t"]
    geo = dict(z_top=z_top, x_back=x_back, W=W)
    c, z_end, z_r = mc_slot_geo(P, geo)
    z_low_head = z_r - P["mc_spacing"] + 0.25 - (P["mc_head"] - MC_HEAD["rn"])
    z_bottom = min(z_top - 36.92 - 2.0, z_low_head - 2.0)
    plate = extrude_xz(round_corners(box(x_back, z_bottom, x_face, z_top), 0.8).union(box(x_back, z_bottom + 1, x_face, z_top)), 0, W)
    add, cut = mount_sanja_riser(P, geo)
    part = plate + add
    for k in range(2):
        part = part + mc_head(P, x_face, c, z_end - k * P["mc_spacing"])
    part = part - cut
    if part.status().name != "NoError":
        raise RuntimeError(f"manifold error: {part.status()}")
    return part, dict(geo, z_bottom=z_bottom)


ADAPTERS = {
    "sanja-riser": adapter_sanja_riser,
    # 以后需要别的场景（比如直接夹在桌边），在这里加
}

# 3MF 里的打印板：(板名, [零件])，零件是 "bracket:<挂载>" 或 "adapter:<场景>"
# 板名和零件名不能有 <>:/\|?*"（Bambu Studio 会报"无效名称"），bambu3mf.check_name 会检查
PLATES = [
    ("一体式 · Sanja 增高架", ["bracket:sanja-riser"]),
    ("Multiconnect 卡座 + Sanja 增高架适配器", ["bracket:multiconnect", "adapter:sanja-riser"]),
    ("Multiconnect 卡座（Multiboard、Skadis、openGrid、墙面）", ["bracket:multiconnect"]),
]


def print_pose(m):
    """侧放：Y=0 那面朝下，放到原点。"""
    m = m.rotate([90, 0, 0])             # (x, y, z) -> (x, -z, y)
    lo = np.asarray(m.bounding_box()[:3])
    return m.translate((-lo).tolist())


def write_3mf(path, plates, title):
    """plates: [(板名, [(名字, 打印姿态的 trimesh, 零件设置)])]；每块板上的零件并排摆在中间。"""
    import bambu3mf
    from render import render
    gap = 10.0
    items, thumbs = [], []
    for p, (_, parts) in enumerate(plates):
        widths = [m.extents[0] for _, m, _ in parts]
        x = -(sum(widths) + gap * (len(parts) - 1)) / 2
        shown = []
        for (name, mesh, settings), w in zip(parts, widths):
            cx = x + w / 2
            items.append(dict(name=name, mesh=mesh, offset=(cx, 0.0), settings=settings, plate=p))
            m = mesh.copy()
            m.apply_translation([cx - (m.bounds[0][0] + m.bounds[1][0]) / 2, -(m.bounds[0][1] + m.bounds[1][1]) / 2, -m.bounds[0][2]])
            shown.append((m, (0.18, 0.55, 0.78)))
            x += w + gap
        buf = io.BytesIO()
        render(shown, (0.55, 0.8, -0.6), size=512, transparent=True).save(buf, format="PNG")
        thumbs.append(buf.getvalue())
    bambu3mf.write_project(path, items, PROJECT_SETTINGS, title=title,
                           description="SEMI-AMS 卡座支架，github.com/pzgz/semi-ams 3d-files/bracket",
                           plate_names=[n for n, _ in plates], thumbnails=thumbs)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--add", action="append", default=None, metavar="STL",
                    help=f"放到带 Sanja 挂齿的打印板上的其他零件（保持它自己的姿态）；不写就用本目录的 {LOCK_STL.name}")
    ap.add_argument("--suffix", default="with-lock", help="带额外零件时 3MF 文件名的后缀")
    ap.add_argument("--assembly", action="store_true", help="另外输出装配姿态的 STL")
    ap.add_argument("--out", default=str(HERE), help="输出目录（默认脚本所在目录）")
    ap.add_argument("--set", nargs="*", default=[], metavar="KEY=VALUE", help="临时修改参数，例如 arm_t=1.8")
    args = ap.parse_args()

    P = dict(DEFAULTS)
    for kv in args.set:
        k, v = kv.split("=", 1)
        if k not in P:
            raise SystemExit(f"未知参数 {k}，可选：{', '.join(P)}")
        P[k] = float(v)

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    meshes = {}
    for mount in MOUNTS:
        part, _ = build(mount, P)
        meshes[f"bracket:{mount}"] = (f"semi-ams-bracket_{mount}", part)
    for name, fn in ADAPTERS.items():
        part, _ = fn(P)
        meshes[f"adapter:{name}"] = (f"semi-ams-adapter_{name}", part)
    printable = {}
    for key, (fname, part) in meshes.items():
        tm = to_trimesh(print_pose(part))
        tm.export(out / f"{fname}.stl")
        printable[key] = (fname, tm, PRINT_SETTINGS)
        e = tm.extents
        print(f"{fname}.stl: {e[0]:.1f} × {e[1]:.1f} × {e[2]:.1f} mm, {tm.volume / 1000:.1f} cm³, watertight={tm.is_watertight}")
        if args.assembly:
            to_trimesh(part).export(out / f"{fname}_assembly.stl")

    # 只出一个 3MF：有锁扣件就放进带 Sanja 挂齿的板（semi-ams-bracket_with-lock.3mf，不进 git），
    # 没有（比如从 GitHub 克隆下来）就出不带锁扣件的 semi-ams-bracket.3mf
    extra_files = args.add if args.add is not None else [str(LOCK_STL)] if LOCK_STL.exists() else []
    plates = [(pname, [printable[k] for k in keys]) for pname, keys in PLATES]
    if extra_files:
        extra = [(Path(f).stem, trimesh.load(f, force="mesh"), ADDED_PART_SETTINGS) for f in extra_files]
        plates = [(pname, parts + (extra if any("sanja-riser" in k for k in keys) else []))
                  for (pname, parts), (_, keys) in zip(plates, PLATES)]
        name = f"semi-ams-bracket_{args.suffix}.3mf"
        note = f"，带 Sanja 挂齿的板上加了 {', '.join(Path(f).name for f in extra_files)}"
    else:
        name, note = "semi-ams-bracket.3mf", f"（没找到 {LOCK_STL.name}，不带锁扣件）"
    write_3mf(out / name, plates, "SEMI-AMS 卡座支架")
    print(f"{name}: {len(plates)} 块打印板{note}")


if __name__ == "__main__":
    main()
