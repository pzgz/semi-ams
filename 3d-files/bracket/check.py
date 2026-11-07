#!/usr/bin/env python3
"""卡座支架和 SEMI-AMS 主体的对位检查 + 预览图。

用法：  python3 check.py <SEMI-AMS-main-body.stl> <obj_2_组合体.stl> [--lock 锁扣件.stl] [--png 目录]
  第一个文件是原厂主体，第二个是原厂 3MF 导出的组合体（从里面取盖板）。
  两个文件都在 Synology Drive 的 3D打印/20260819_semi-ams/ 里。

检查内容：
  一体式和 Multiconnect 卡座：和主体/盖板的干涉、六边形凸起和 B 孔的间隙、下臂凸点过盈、磁铁槽对位
  Multiconnect：卡座和 Sanja 适配器的配合、拔出时的卡点、标准圆连接头（R10）在槽里的配合
  锁扣件（给了 --lock 时）：插进锁扣孔是否干涉
  打印姿态：需要支撑的朝下面
"""
import argparse
import math
import sys
from pathlib import Path

import numpy as np
import trimesh
from manifold3d import Manifold, Mesh

sys.path.insert(0, str(Path(__file__).resolve().parent))
import bracket as B  # noqa: E402
from render import render  # noqa: E402


def to_manifold(tm):
    tm = tm.copy()
    tm.merge_vertices()
    return Manifold(Mesh(vert_properties=np.asarray(tm.vertices, np.float32), tri_verts=np.asarray(tm.faces, np.uint32)))


def load_device(body_path, combo_path):
    body = trimesh.load(body_path)
    body.apply_translation(-body.bounds[0])
    # 主体 (x, y, z) -> 装配 (X=x, Y=z, Z=82.5-y)
    T_body = np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 82.5], [0, 0, 0, 1]], float)
    body.apply_transform(T_body)
    lid = None
    if combo_path:
        parts = trimesh.load(combo_path).split(only_watertight=False)
        cand = [p for p in parts if abs(p.extents[0] - 56.86) < 0.1 and abs(p.extents[1] - 82.5) < 0.1 and p.extents[2] < 6]
        if cand:
            lid = cand[0]
            lid.apply_translation(-lid.bounds[0])
            # 盖板 (x, y, z) -> 装配 (X=x, Y=25.5-z, Z=y)：外表面 z=0 在 Y=25.5
            T_lid = np.array([[1, 0, 0, 0], [0, 0, -1, 25.5], [0, 1, 0, 0], [0, 0, 0, 1]], float)
            lid.apply_transform(T_lid)
    return body, lid


def std_head(x_face, c, zc):
    """标准 Multiconnect 圆连接头（R10，直段 1.0，45° 倒角 2.5，颈 R7.5 × 0.5），平贴在 x_face 所在的板面上。"""
    from manifold3d import CrossSection
    prof = [(0, 0), (10, 0), (10, 1), (7.5, 3.5), (7.5, 4.0), (0, 4.0)]
    head = Manifold.revolve(CrossSection([prof]), circular_segments=96)       # 轴沿 z，z=0 是头顶
    return head.rotate([0, 90, 0]).mirror([1, 0, 0]).translate([x_face + 4.0, c, zc])


def place_lock(lock_path, x_back, z_top, P):
    """把锁扣件按设计方向插进锁扣孔（尖的一边朝下），返回 trimesh。"""
    L = trimesh.load(lock_path, force="mesh")
    L.apply_translation(-L.bounds[0])
    T = np.eye(4); T[:3, :3] = [[0, 0, 1], [0, -1, 0], [1, 0, 0]]
    L.apply_transform(T)
    cc = T[:3, :3] @ np.array([8.23, 0, 7.59])                               # 实测圆柱中心
    L.apply_translation([x_back - 2.50 - cc[0], 0, z_top - 4.04 - cc[2]])
    L.apply_translation([0, P["keyhole_stop"] + 0.2 - L.bounds[0][1], 0])
    return L


def overhangs(mesh):
    """打印姿态下和水平面夹角小于 30°（Bambu 默认支撑阈值）的朝下面，返回 [(面积, 宽, 长)]。"""
    n, a, cz = mesh.face_normals, mesh.area_faces, mesh.triangles_center[:, 2]
    idx = np.where((n[:, 2] < -0.866) & (cz > 0.25))[0]
    if not len(idx):
        return []
    out = []
    for p in mesh.submesh([idx], append=True).split(only_watertight=False):
        e = p.extents
        if p.area >= 0.5:
            out.append((p.area, min(e[0], e[1]), max(e[0], e[1])))
    return sorted(out, reverse=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("body"); ap.add_argument("combo", nargs="?")
    ap.add_argument("--lock", help="锁扣件 STL（reinforced locking piece even thicker.stl）")
    ap.add_argument("--png", help="输出预览图的目录")
    args = ap.parse_args()

    P = dict(B.DEFAULTS)
    body, lid = load_device(args.body, args.combo)
    mbody = to_manifold(body)
    mlid = to_manifold(lid) if lid is not None else None
    parts = {m: B.build(m, P) for m in B.MOUNTS}
    adapter, ageo = B.adapter_sanja_riser(P)

    for mount, (part, geo) in parts.items():
        print(f"== {mount}：干涉检查（装配姿态）==")
        v_body = (part ^ mbody).volume()
        print(f"  卡座 ∩ 主体: {v_body:.2f} mm³" + (f"，卡座 ∩ 盖板: {(part ^ mlid).volume():.2f} mm³" if mlid else ""))
        pad_zone = Manifold.cube([P["arm_len"], P["width"], P["preload"] + 0.01])
        print(f"  其中下臂凸点过盈（设计值 {P['preload']} mm）: {(part ^ pad_zone ^ mbody).volume():.2f} mm³")
    wall_web = -P["mag_depth"] - (-P["wall_mc"] + B.MC_SLOT["depth"])
    print(f"  Multiconnect 槽底到磁铁槽底: {wall_web:.2f} mm")

    print("== B 孔和六边形凸起 ==")
    r_c = P["boss_ac"] / 2
    print(f"  B 孔 Ø{B.DEV['b_d']}，凸起对角 {P['boss_ac']}（单边间隙 {B.DEV['b_d'] / 2 - r_c:.2f}），对边 {P['boss_ac'] * math.sqrt(3) / 2:.2f}，高 {P['boss_h']}")
    sec = body.section(plane_origin=(0, 0, B.DEV["h"] - 1.0), plane_normal=(0, 0, 1))
    if sec is not None:
        for e in sec.discrete:
            cxy = e[:, :2].mean(0)
            if np.linalg.norm(cxy - [B.DEV["b_x"], B.DEV["b_y"]]) < 1.0:
                print(f"  实测 B 孔中心 ({cxy[0]:.2f}, {cxy[1]:.2f})，直径 {2 * np.linalg.norm(e[:, :2] - cxy, axis=1).mean():.2f}")

    print("== 磁铁槽对位（支架槽中心 vs 主体磁铁孔中心）==")
    sec = body.section(plane_origin=(0.5, 0, 0), plane_normal=(1, 0, 0))
    holes = []
    for e in sec.discrete:
        c2 = e[:, 1:].mean(0); rr = np.linalg.norm(e[:, 1:] - c2, axis=1)
        if rr.std() < 0.05 and 2.8 < rr.mean() < 3.3:
            holes.append(c2)
    for (my, mz) in B.DEV["magnets"]:
        d = min(np.linalg.norm(h - [my, mz]) for h in holes) if holes else float("nan")
        print(f"  ({my:.2f}, {mz:.2f}) 偏差 {d:.3f} mm")

    cradle, geo = parts["multiconnect"]
    c, z_end, z_r = B.mc_slot_geo(P, geo)
    print("== Multiconnect 卡座 + Sanja 适配器 ==")
    print(f"  装到位时干涉: {(cradle ^ adapter).volume():.3f} mm³；适配器 ∩ 主体: {(adapter ^ mbody).volume():.3f} mm³")
    print("  把卡座往上提（拔出方向）时连接头挤过 v2 卡点：")
    print("   " + "  ".join(f"{dz}mm:{(cradle.translate([0, 0, dz]) ^ adapter).volume():.2f}" for dz in (0.5, 1, 2, 4, 6, 8, 10, 12)) + "  (mm³)")
    print("== 标准圆连接头（R10）在卡座槽里 ==")
    zc = z_end - 10.0
    for name, z in [("顶到槽顶（装到位）", zc), ("往下 2", zc - 2), ("往下 4", zc - 4), ("往下 8", zc - 8),
                    ("第二个，间距 25（Multiboard）", zc - 25), ("第二个，间距 40（Skadis）", zc - 40)]:
        print(f"  {name:<24s} 干涉 {(cradle ^ std_head(geo['x_back'], c, z)).volume():.3f} mm³")
    print(f"  间距 40 时下面那个连接头离槽下口 {(zc - 40 - 10) - geo['z_bot']:.1f} mm")

    if args.lock:
        print("== 锁扣件 ==")
        for name, (xb, zt, part) in {"一体式": (parts["sanja-riser"][1]["x_back"], parts["sanja-riser"][1]["z_top"], parts["sanja-riser"][0]),
                                     "适配器": (ageo["x_back"], ageo["z_top"], adapter)}.items():
            Lt = place_lock(args.lock, xb, zt, P)
            print(f"  插进{name}的锁扣孔: 干涉 {(part ^ to_manifold(Lt)).volume():.3f} mm³")

    print("== 打印姿态（侧放）下需要支撑的朝下面 ==")
    named = {"一体式": parts["sanja-riser"][0], "Multiconnect 卡座": cradle, "Sanja 适配器": adapter}
    for name, part in named.items():
        oh = overhangs(B.to_trimesh(B.print_pose(part)))
        widest = max((w for _, w, _ in oh), default=0)
        print(f"  {name}: {sum(a for a, _, _ in oh):.0f} mm²，最宽 {widest:.1f} mm（都是短跨度的桥接/小悬空）" if oh else f"  {name}: 无")

    if args.png:
        render_images(Path(args.png), P, parts, adapter, ageo, body, lid, args.lock)


def render_images(out, P, parts, adapter, ageo, body, lid, lock_path):
    from PIL import Image
    out.mkdir(parents=True, exist_ok=True)
    orange, teal, red = (0.96, 0.62, 0.28), (0.20, 0.62, 0.78), (0.86, 0.38, 0.36)
    devs = [(body, orange)] + ([(lid, (0.98, 0.78, 0.52))] if lid is not None else [])
    br = B.to_trimesh(parts["sanja-riser"][0])
    cr = B.to_trimesh(parts["multiconnect"][0])
    ad = B.to_trimesh(adapter)

    def pair(a, b, name):
        im = Image.new("RGB", (840, 420), "white")
        im.paste(a.resize((420, 420), Image.LANCZOS), (0, 0)); im.paste(b.resize((420, 420), Image.LANCZOS), (420, 0))
        im.save(out / name)

    # 一体式
    pair(render([(br, teal)] + devs, (1, 1, -0.55), size=840), render([(br, teal)] + devs, (1, -1, -0.45), size=840), "assembly.png")
    pair(render([(br, teal)], (0.9, 1, -0.6), size=840), render([(br, teal)], (1, -0.9, -0.5), size=840), "bracket.png")
    section_figure(out / "section.png", P, body, [(br, "#4a9cc6", "#1d5f80")], B.DEV["b_y"], [
        ("六边形凸起插进 B 孔", (B.DEV["b_x"], B.DEV["h"] - 1.0), (30, 86)),
        ("上臂（硬）", (P["top_len"] - 4, B.DEV["h"] + P["top_t"] / 2), (30, 95)),
        ("锁扣孔（插锁扣件）", (-7.2, B.DEV["h"] + P["top_t"] - 4.0), (-48, 90)),
        ("法式挂齿 ×2，挂进增高架侧面的槽", (-7.5, 46), (-48, 30)),
        ("磁铁槽 ×3（虚线是投影位置，Ø6.2×3.1）", (-2.0, 60.12), (-48, 62)),
        ("下臂（弹性）+ 末端凸点\n预压 0.5mm，把主体往上顶", (P["arm_len"] - 3, -1.2), (24, -14)),
        ("SEMI-AMS 主体（剖面）", (40, 40), (40, 60)),
        ("进料接头一侧（朝上）", (42, 80), (52, 92)),
        ("缓冲管出口一侧（朝下）", (40, 0), (52, -8)),
    ], f"一体式：过 B 孔中心的剖面（Y = {B.DEV['b_y']}），左侧贴增高架", magnets_x0=0)
    # Multiconnect
    geo = parts["multiconnect"][1]
    c, z_end, z_r = B.mc_slot_geo(P, geo)
    pair(render([(cr, teal), (ad, red)] + devs, (1, 1, -0.55), size=840), render([(cr, teal), (ad, red)] + devs, (1, -1, -0.45), size=840), "mc_assembly.png")
    pair(render([(cr, teal)], (1, -0.6, -0.4), size=840), render([(ad, red)], (-1, -0.7, -0.5), size=840), "mc_parts.png")
    section_figure(out / "mc_section.png", P, body, [(cr, "#4a9cc6", "#1d5f80"), (ad, "#e07a73", "#8c2f2a")], c, [
        ("卡座背面的 Multiconnect 槽\n（下端开口，上端平顶）", (geo["x_back"] + 2, 30), (-62, 10)),
        ("适配器上的连接头 ×2\n（间距 25）", (geo["x_back"] + 2, z_end - 5), (-62, 52)),
        ("适配器：挂齿 + 锁扣孔\n（和一体式一样）", (ageo["x_back"] - 3, 46), (-62, 32)),
        ("卡座（背板 8.3mm）", (-3, 20), (12, -12)),
        ("SEMI-AMS 主体（剖面）", (40, 40), (40, 60)),
    ], f"Multiconnect 版：过槽中心的剖面（Y = {c}）", xlim=(-66, 72))
    # 打印板缩略图
    import zipfile, io
    here = Path(B.__file__).parent
    z = zipfile.ZipFile(next(p for p in (here / "semi-ams-bracket_with-lock.3mf", here / "semi-ams-bracket.3mf") if p.exists()))
    names = sorted(n for n in z.namelist() if n.startswith("Metadata/plate_") and n.endswith(".png"))
    im = Image.new("RGB", (280 * len(names), 280), (60, 60, 64))
    for i, n in enumerate(names):
        t = Image.open(io.BytesIO(z.read(n))).convert("RGBA").resize((280, 280), Image.LANCZOS)
        bg = Image.new("RGBA", t.size, (60, 60, 64, 255)); bg.alpha_composite(t)
        im.paste(bg.convert("RGB"), (280 * i, 0))
    im.save(out / "plates.png")
    print(f"预览图已写入 {out}")


def section_figure(path, P, body, parts, plane_y, ann, title, xlim=(-50, 72), magnets_x0=None):
    """过 Y = plane_y 的剖面，parts: [(trimesh, 填充色, 边线色)]，ann: [(文字, 箭头指向, 文字位置)]。"""
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib import font_manager
    for f in ["PingFang SC", "Noto Sans CJK SC", "Hiragino Sans GB", "Microsoft YaHei"]:
        if any(f in x.name for x in font_manager.fontManager.ttflist):
            plt.rcParams["font.family"] = f
            break
    fig, ax = plt.subplots(figsize=(8.4, 8.4), dpi=100)

    def fill(mesh, fc, ec):
        s = mesh.section(plane_origin=(0, plane_y, 0), plane_normal=(0, 1, 0))
        if s is None:
            return
        p2, _ = s.to_2D(to_2D=np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, -1, 0, 0], [0, 0, 0, 1]], float), normal=(0, 1, 0), check=False)
        for poly in p2.polygons_full:
            xs, zs = poly.exterior.xy
            ax.fill(xs, zs, fc=fc, ec=ec, lw=0.8)
            for hole in poly.interiors:
                hx, hz = hole.xy
                ax.fill(hx, hz, fc="white", ec=ec, lw=0.8)

    fill(body, "#f6c79a", "#b8742f")
    for mesh, fc, ec in parts:
        fill(mesh, fc, ec)
    if magnets_x0 is not None:
        for (_, mz) in B.DEV["magnets"]:
            r = P["mag_d"] / 2
            ax.plot([0, -P["mag_depth"], -P["mag_depth"], 0], [mz - r, mz - r, mz + r, mz + r], color="#0b3a52", lw=1.0, ls="--")
    for text, xy, xt in ann:
        ax.annotate(text, xy=xy, xytext=xt, fontsize=10, ha="left", arrowprops=dict(arrowstyle="-", color="#555", lw=0.7))
    ax.set_xlim(*xlim); ax.set_ylim(-20, 100)
    ax.set_aspect("equal"); ax.axis("off")
    ax.set_title(title, fontsize=12)
    plt.tight_layout(); fig.savefig(path, dpi=100); plt.close(fig)


if __name__ == "__main__":
    main()
