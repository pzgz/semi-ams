# SEMI-AMS

SEMI-AMS 耗材进料器（拓竹打印机外置料盘的自动送料、缓冲）相关的全部内容：替换固件、主控、3D 打印件。

## 目录

| 目录 | 内容 | 状态 |
|---|---|---|
| [`firmware/`](firmware/) | 进料器控制板（ESP-12S）的替换固件，PlatformIO 工程 | v1.3.0，已在拓竹 P2S 上实测 |
| [`controller/`](controller/) | 主控 | 方案待定（ESP32 或纯软件），目前只是占位 |
| [`3d-files/`](3d-files/) | 自己设计、修改的打印件（支架、外壳调整等） | 卡座支架（一体式 Sanja 增高架版、Multiconnect 卡座 + 适配器）已出图，待试打 |

## 固件

```bash
cd firmware
bash host_test/run.sh          # 仿真测试，不需要开发板
pio run                        # 编译
pio run -t upload              # 串口烧录
pio run -e ota -t upload       # 无线升级（设备 IP 在 platformio.ini 的 [env:ota]，也可以加 --upload-port IP）
```

详细说明见 [firmware/README.md](firmware/README.md) 和 [firmware/docs/](firmware/docs/)。PlatformIO 命令都要在 `firmware/` 里执行；用 VS Code 的 PlatformIO 插件时，打开 `firmware/` 这个文件夹（`platformio.ini` 所在的目录），不要打开仓库根目录。

## 原厂资料

原厂的 PCB（Gerber、网表）、BOM、打印件（STL / 3MF）、烧录教程和原厂固件放在本机 Synology Drive 的 `3D打印/20260819_semi-ams/`，只作参考，不放进仓库。

## 不要提交的文件

根目录的 `.gitignore` 对所有子目录生效，已排除 `.pio/`、`.vscode/`、`logs/`、`.DS_Store`，以及 `firmware/tools/bambu_listen.py` 录下的 `bambu_log_*.jsonl`。这些记录里有打印机序列号、拓竹云的会话密钥和证书，不要外传。
