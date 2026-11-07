#!/usr/bin/env python3
"""出 README 用的预览图：images/shell.png、images/cover.png、images/assembly.png、images/sections.png。

用法：  python3 render_images.py [原厂目录]      （先跑 enclosure.py 生成 STL）
渲染器用 ../bracket/render.py（numpy 光栅，不依赖 OpenGL）。
"""
import sys
from pathlib import Path

import matplotlib
import numpy as np
import trimesh

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from PIL import Image, ImageDraw, ImageFont  # noqa: E402

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.path.insert(0, str(HERE.parent / "bracket"))
import enclosure as E  # noqa: E402
from render import render  # noqa: E402

from matplotlib import font_manager  # noqa: E402

_have = {f.name for f in font_manager.fontManager.ttflist}
plt.rcParams["font.family"] = [f for f in ["PingFang SC", "Noto Sans CJK SC", "Microsoft YaHei"] if f in _have] + ["sans-serif"]
plt.rcParams["axes.unicode_minus"] = False

SHELL_C = (0.30, 0.31, 0.34)
COVER_C = (0.80, 0.80, 0.78)
BODY_C = (0.96, 0.62, 0.30)
PCB_C = (0.15, 0.45, 0.30)
OLD_C = (0.62, 0.70, 0.82)


def font(size):
    for f in ["/System/Library/Fonts/PingFang.ttc", "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc"]:
        if Path(f).exists():
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def panel(images, captions, out, size=700):
    W = size * len(images)
    canvas = Image.new("RGB", (W, size + 80), "white")
    d = ImageDraw.Draw(canvas)
    for i, (im, cap) in enumerate(zip(images, captions)):
        canvas.paste(im.convert("RGB"), (i * size, 0))
        d.text((i * size + size / 2, size + 45), cap, fill=(40, 40, 40), font=font(26), anchor="mm")
    canvas.save(out)


def pcb_mesh():
    return E.to_trimesh(E.extrude(E.rbox(*E.PCB_RECT, 3.0), -E.P["pcb_t"], 0))


def main():
    src = Path(sys.argv[1]).expanduser() if len(sys.argv) > 1 else E.DEFAULT_SRC
    img = HERE / "images"
    img.mkdir(exist_ok=True)
    body = trimesh.load(src / "SEMI-AMS-main-body.stl")
    shell0 = trimesh.load(src / "SEMI-AMS-PCB保护壳.stl")
    cover0 = trimesh.load(src / "SEMI-AMS-cover.stl")
    shell = trimesh.load(HERE / "semi-ams-pcb-shell_full.stl")
    cover = trimesh.load(HERE / "semi-ams-cover_thick.stl")
    zb = 9.0 + E.P["pcb_t"]

    # 1. PCB 壳：外面 / 里面 / 原壳对比
    old = shell0.copy(); old.apply_translation((E.SHELL_OFF[0], E.SHELL_OFF[1], -zb))
    s_on = shell.copy(); s_on.apply_translation((0, 0, -zb))
    a = render([(body, BODY_C), (s_on, SHELL_C)], (-0.35, 0.5, 1.0), up=(0, 1, 0))   # 装在主体上，从外面（-z）看
    b = render([(shell, SHELL_C)], (0.35, 0.5, -1.0), up=(0, 1, 0))          # 从里面（+z）看
    c = render([(body, BODY_C), (old, SHELL_C), (pcb_mesh(), PCB_C)], (-0.35, 0.5, 1.0), up=(0, 1, 0))
    panel([c, a, b], ["原来：只罩住 PCB", "新壳：整面罩住（缺口在边上）", "新壳：里面"], img / "shell.png")

    # 2. 盖板：外面 / 原来 / 里面（按装好后的方向摆：盖板 y 朝下，字是正的）
    c0 = cover0.copy()
    a = render([(cover, COVER_C)], (0.35, -0.5, 1.0), up=(0, -1, 0))
    b = render([(c0, COVER_C)], (0.35, -0.5, 1.0), up=(0, -1, 0))
    d = render([(cover, COVER_C)], (-0.35, -0.5, -1.0), up=(0, -1, 0))
    panel([b, a, d], ["原来：3 mm", "加厚后：外面（开口沉孔）", "加厚后：里面（不变）"], img / "cover.png")

    # 3. 装配：PCB 那一面 / 盖板那一面
    s = shell.copy(); s.apply_translation((0, 0, -zb))
    T = np.array([[1, 0, 0, 0], [0, -1, 0, 82.5], [0, 0, -1, 25.5 + E.P["cover_add"]], [0, 0, 0, 1]], float)
    cv = cover.copy(); cv.apply_transform(T); cv.fix_normals()
    a = render([(body, BODY_C), (s, SHELL_C), (cv, COVER_C)], (-0.6, 0.55, 0.8), up=(0, 1, 0))
    b = render([(body, BODY_C), (s, SHELL_C), (cv, COVER_C)], (0.6, 0.55, -0.8), up=(0, 1, 0))
    panel([a, b], ["装好：PCB 那一面", "装好：盖板那一面"], img / "assembly.png", size=800)

    # 4. 剖面：PCB 壳过 ④①② 三个孔（y=79），盖板过下面两个孔（y=3.5）
    fig, axs = plt.subplots(2, 1, figsize=(12, 6.2))
    ax = axs[0]
    for mesh, col, dz in [(body, BODY_C, 0), (pcb_mesh(), PCB_C, 0), (shell, SHELL_C, -zb)]:
        m = mesh.copy(); m.apply_translation((0, 0, dz))
        sec = m.section(plane_origin=[0, 79.0, 0], plane_normal=[0, 1, 0])
        if sec is None:
            continue
        p2, _ = sec.to_2D(to_2D=np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], float))
        for q in p2.polygons_full:
            xs, ys = q.exterior.xy
            ax.fill(56.864 - np.array(xs), -np.array(ys), color=col, ec="k", lw=0.4)
            for r in q.interiors:
                xs, ys = r.xy
                ax.fill(56.864 - np.array(xs), -np.array(ys), color="white", ec="k", lw=0.4)
    ax.set_ylim(-6, 12); ax.set_xlim(-2, 59); ax.set_aspect("equal")
    ax.set_title("PCB 壳剖面（过 ②①④ 三个孔，从外面看，外面朝上）：PCB 上的压片 1 mm，主体上的压片 2.6 mm，顶面齐平", fontsize=11)
    for x, t in [(3.5, "②"), (28.36, "①"), (53.36, "④")]:
        ax.text(x, 11, t, ha="center", fontsize=13)
    ax = axs[1]
    for mesh, col, dz in [(cover, COVER_C, -E.P["cover_add"]), (cover0, OLD_C, 9)]:
        m = mesh.copy(); m.apply_translation((0, 0, dz))
        sec = m.section(plane_origin=[0, 3.5, 0], plane_normal=[0, 1, 0])
        p2, _ = sec.to_2D(to_2D=np.array([[1, 0, 0, 0], [0, 0, 1, 0], [0, 1, 0, 0], [0, 0, 0, 1]], float))
        for q in p2.polygons_full:
            xs, ys = q.exterior.xy
            ax.fill(56.864 - np.array(xs), -np.array(ys), color=col, ec="k", lw=0.4)
    ax.text(60, -10.5, "原来", fontsize=11, va="center"); ax.text(60, -1.5, "加厚后", fontsize=11, va="center")
    ax.set_ylim(-15, 5); ax.set_xlim(-2, 66); ax.set_aspect("equal")
    ax.set_title("盖板剖面（过下面两个角孔，外面朝上）：外侧加 3 mm，沉孔 Ø6.2 深 3.5、朝外边开口", fontsize=11)
    for a_ in axs:
        a_.axis("off")
    plt.tight_layout()
    plt.savefig(img / "sections.png", dpi=110)
    print("images/ 已更新")


if __name__ == "__main__":
    main()
