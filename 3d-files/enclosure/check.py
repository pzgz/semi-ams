#!/usr/bin/env python3
"""检查：新 PCB 壳 / 新盖板装到原厂主体上的干涉、对位，以及盖板朝内一面有没有被改动。

用法：  python3 check.py [原厂目录]
"""
import sys
from pathlib import Path

import numpy as np
import trimesh
from shapely.geometry import Point

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import enclosure as E  # noqa: E402


def inter_vol(a, b):
    return (E.to_manifold(a) ^ E.to_manifold(b)).volume()


def main():
    src = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else E.DEFAULT_SRC
    body = trimesh.load(src / "SEMI-AMS-main-body.stl")
    shell0 = trimesh.load(src / "SEMI-AMS-PCB保护壳.stl")
    cover0 = trimesh.load(src / "SEMI-AMS-cover.stl")
    shell = trimesh.load(HERE / "semi-ams-pcb-shell_full.stl")
    cover = trimesh.load(HERE / "semi-ams-cover_thick.stl")
    zb = 9.0 + E.P["pcb_t"]
    ok = True

    def report(name, v, lim=0.01):
        nonlocal ok
        flag = "OK " if v <= lim else "!! "
        ok &= v <= lim
        print(f"  {flag}{name}: {v:.3f} mm³")

    print("PCB 壳（装到主体上：外表面在 z=-10.6，口沿贴主体 z=0）")
    s = shell.copy(); s.apply_translation((0, 0, -zb))
    pcb = E.extrude(E.rbox(*E.PCB_RECT, 3.0), -E.P["pcb_t"], 0)
    motor = trimesh.creation.cylinder(radius=12.2, height=34, sections=96,
                                      transform=trimesh.transformations.rotation_matrix(np.pi / 2, (1, 0, 0)))
    motor.apply_translation((14.5, 57.5, 8.75))          # 电机槽圆弧拟合出来的圆心，Ø24.4 的 370 电机
    report("和主体干涉", inter_vol(s, body))
    report("和 PCB（1.6 板）干涉", (E.to_manifold(s) ^ pcb).volume())
    report("和电机（Ø24.4 圆柱）干涉", inter_vol(s, motor))
    gap = (-motor.bounds[0][2]), (zb - 1.0)
    print(f"  电机凸出主体 {gap[0]:.1f} mm，壳内高 {gap[1]:.1f} mm，余 {gap[1] - gap[0]:.1f} mm")
    out = s.bounds
    print(f"  外形 x {out[0][0]:.2f}~{out[1][0]:.2f}  y {out[0][1]:.2f}~{out[1][1]:.2f}（主体 0~56.86, 0~82.50）")
    # 每个螺丝孔下面的压片：底面贴哪里
    for h in E.PCB_HOLES + list(E.BODY_HOLES):
        x, y = h[0] + 2.4, h[1]
        loc, _, _ = shell.ray.intersects_location([[h[0], h[1] - 2.4, -1]], [[0, 0, 1]])
        zs = sorted(set(np.round(loc[:, 2], 2)))
        print(f"  孔 {h}: 压片 z {zs}（外表面 0，压片顶 {E.P['tab_top']}，贴 PCB {9.0}，贴主体 {zb}）")
    # 原壳的部分是否原样：比较外表面那 1 mm 的刻字/按钮/LED
    print("原壳部分对照")
    for z in (0.25, 0.75):
        a = E.unary_union(E.sections(shell, z))
        o = shell0.copy(); o.apply_translation((*E.SHELL_OFF, 0))
        b = E.unary_union(E.sections(o, z))
        holes_a = sorted(round(E.Polygon(r).area, 1) for q in E.polys(a) for r in q.interiors)
        holes_b = sorted(round(E.Polygon(r).area, 1) for q in E.polys(b) for r in q.interiors)
        print(f"  z={z}: 新壳顶板上的洞 {holes_a}")
        print(f"         原壳顶板上的洞 {holes_b}")

    print("盖板（装到主体上：y→82.5-y, z→25.5-z）")
    c = cover.copy(); c.apply_translation((0, 0, -E.P["cover_add"]))
    text = E.build_cover(cover0)[1]["text"].area
    for z in (0.75, 1.5, 2.5, 3.2, 4.0, 5.0):
        a = E.unary_union(E.sections(c, z)); b = E.unary_union(E.sections(cover0, z))
        d = a.symmetric_difference(b).area
        if z < 1.0:   # 原刻字（0~1 mm）填平了，差的应该正好是刻字面积
            print(f"  {'OK ' if abs(d - text) < 0.5 else '!! '}z={z}: 新旧截面差 {d:.1f} mm²（= 填平的原刻字 {text:.1f} mm²）")
        else:
            print(f"  {'OK ' if d < 0.05 else '!! '}z={z}（原外表面往里）: 新旧截面差 {d:.3f} mm²")
    T = np.array([[1, 0, 0, 0], [0, -1, 0, 82.5], [0, 0, -1, 25.5], [0, 0, 0, 1]], float)
    c2 = c.copy(); c2.apply_transform(T); c2.fix_normals()
    o2 = cover0.copy(); o2.apply_transform(T); o2.fix_normals()
    v_new, v_old = inter_vol(c2, body), inter_vol(o2, body)
    report(f"和主体干涉（原盖板是 {v_old:.3f}，相同即可）", abs(v_new - v_old))
    print(f"  厚度 {cover.extents[2]:.2f}（原 {cover0.extents[2]:.2f}），外表面到主体 {3 + E.P['cover_add']:.1f} mm")
    for h in E.COVER_HOLES:
        loc, _, _ = cover.ray.intersects_location([[h[0] + 2.3, h[1], -1]], [[0, 0, 1]])
        print(f"  沉孔 {h}: 孔边 2.3 mm 处第一个实体面 z={loc[:, 2].min():.2f}（沉孔深 {E.P['cb_depth']}）")
    print("网格（按位置合并顶点后再查，切片软件读 STL 就是这样）")
    for name in ("semi-ams-pcb-shell_full.stl", "semi-ams-cover_thick.stl"):
        t = trimesh.load(HERE / name)
        _, cnt = np.unique(t.edges_sorted, axis=0, return_counts=True)
        bad = int((cnt != 2).sum()); parts = len(t.split(only_watertight=False))
        good = t.is_watertight and bad == 0 and parts == 1
        ok &= good
        print(f"  {'OK ' if good else '!! '}{name}: 封闭={t.is_watertight}，非流形边 {bad}，实体 {parts} 个，{len(t.faces)} 个三角形")
    print("全部通过" if ok else "有问题")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
