# 3D 打印件

自己设计或修改的打印件，例如支架、外壳调整。

原厂的打印件在本机 Synology Drive 的 `3D打印/20260819_semi-ams/`（`SEMI-AMS-main-body.stl`、`SEMI-AMS直.3mf`、`SEMI-AMS直_stls/`、`ams架.stl` 等），只作参考；要改的话复制过来再改。

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
