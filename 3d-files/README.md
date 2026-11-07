# 3D 打印件

自己设计或修改的打印件，例如支架、外壳调整。

原厂的打印件在本机 Synology Drive 的 `3D打印/20260819_semi-ams/`（`SEMI-AMS-main-body.stl`、`SEMI-AMS直.3mf`、`SEMI-AMS直_stls/`、`ams架.stl` 等），只作参考；要改的话复制过来再改。

## 现有零件

| 文件夹 | 内容 |
|---|---|
| [`bracket/`](bracket/) | 卡座支架：代替原厂磁吸板，"[" 形卡住主体（六边形凸起插 B 孔 + 弹性下臂 + 磁铁），背后两种挂法：一体式直接挂 Sanja 3D P2S 增高架；或 Multiconnect 卡座 + 适配器（Sanja 增高架用这里的适配器，Multiboard / Skadis / openGrid / 墙面用现成的 Multiconnect 连接件） |
| [`enclosure/`](enclosure/) | 外壳改版：PCB 保护壳扩大到罩住主体整面（原 PCB 三颗螺丝照旧压 PCB，主体上另外三个孔加热熔螺母压壳）；另一面的盖板外侧加厚到 6 mm，螺丝孔做朝外边开口的沉孔；附带 P2S 打印参数的 Bambu Studio 工程 |

## 建议的放法

每个零件一个文件夹：

```
3d-files/
  <零件名>/
    README.md    用途、改了什么、打印参数（材料、层高、支撑、朝向）
    源文件       能改尺寸的那种，例如 STEP 或建模软件的工程文件
    *.3mf        切片工程（可选）
    *.stl        导出的模型
```

- 尽量连源文件一起放，只有 STL 以后不好改。
- 文件很大或很多时，可以考虑 Git LFS（`git lfs track "*.stl" "*.3mf" "*.step"`），目前没有启用。
