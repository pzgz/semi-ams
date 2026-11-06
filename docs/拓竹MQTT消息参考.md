# 拓竹 P2S 本地 MQTT 消息参考（实测整理）

| 项目 | 内容 |
|---|---|
| 打印机 | Bambu Lab P2S，固件 01.03.00.00；AMS 2 Pro 一台（固件 05.00.22.22）；喷嘴 HH01 0.4mm |
| 打印机模式 | 正常连接拓竹云（**没有**开开发者模式，也没有切到仅局域网模式） |
| 采集日期 | 2026-11-05，三段记录共 5954 条消息；2026-11-06 补充了两台 SEMI-AMS（v1.2.x）日志里看到的错误码 |
| 采集工具 | `tools/bambu_listen.py`（只读，连接时发一次 `pushall`），原始记录 `bambu_log_*.jsonl` |
| 用途 | SEMI-AMS 固件判断开始送料、退料的依据；以后做多台协同（见 [多台SEMI-AMS协同方案](多台SEMI-AMS协同方案.md)）、显示打印进度时查字段 |

本文只写实测看到的内容。字段含义标"推测"的，是根据前后变化推出来的，没有官方资料证实；标"参考 ha-bambulab"的，取自 Home Assistant 拓竹插件的常量表，没有逐一核实。

**原始记录不要外传**：里面有打印机序列号、拓竹云视频直播的会话密钥（`liveview` 消息）、带签名的文件下载链接和证书。`.gitignore` 已经排除 `bambu_log_*.jsonl`。本文中的序列号等已替换成 `<序列号>`。

---

## 1. 连接

| 项目 | 实测情况 |
|---|---|
| 地址 | `<打印机IP>:8883`，TLS，打印机用自签名证书（客户端不校验证书） |
| 登录 | 用户名 `bblp`，密码是打印机屏幕上的访问码（配对码）；客户端 ID 随意 |
| 订阅 | `device/<序列号>/report`（打印机发出的所有消息） |
| 发送 | `device/<序列号>/request`。SEMI-AMS 只发 `pushall`（请求完整状态），不发控制命令 |
| 云模式下可用 | 是。打印机连着拓竹云，局域网 MQTT 照样能读 |
| 同时连接 | SEMI-AMS 和电脑上的监听程序同时连接没有问题（至少 2 个客户端）。上限没测 |
| TLS 分片协商（MFLN） | 支持。客户端可以只用 1KB 接收缓冲（ESP8266 必须用到这一点，否则要 16KB） |
| 消息频率 | `print` 消息**每秒一条**，几乎每条都是完整状态（平均 7.8KB，最大 8.2KB）。比旧资料里"P 系列只发变化的字段"多得多 |

### 1.1 消息种类（按顶层键）

| 顶层键 | 条数 | 大小 | 内容 |
|---|---|---|---|
| `print` | 4576 | 100–8208 字节 | 打印状态 `push_status`（绝大多数），以及打印机收到的命令回报（见第 3 节） |
| `system` | 1342 | 约 200 字节 | `ledctrl`：灯光控制回报（`chamber_light` 腔体灯） |
| `info` | 19 | 1.3KB | `get_version`：各模块的型号、序列号、固件版本 |
| `liveview` | 11 | 0.5–1.4KB | 视频直播会话（**含密钥**） |
| `camera` | 4 | 240 字节 | `ipcam_get_media_info`：延时摄影存储空间 |
| `security` | 2 | 0.3–1.2KB | `app_cert_list` / `app_cert_install`：证书 |

---

## 2. 打印状态 `push_status`

`{"print":{"command":"push_status", ...}}`。下面按用途分组，只列有用或者已经搞清楚的字段。

### 2.1 打印进度

| 字段 | 实测值 | 说明 |
|---|---|---|
| `gcode_state` | `FAILED`、`PREPARE`、`RUNNING`、`FINISH` | 打印状态。取消后停在 `FAILED`、打完停在 `FINISH`，直到下一次打印；**没有见到 `IDLE`**。`PREPARE` 只持续 2–4 秒 |
| `stg_cur` | 见 2.2 节 | 当前阶段，`-1` = 不在打印，`0` = 正在打印 |
| `stg` | `[2,13,11,4,8,14,3,54,1,51]` | 这次任务要经过的阶段列表 |
| `mc_print_stage` / `mc_stage` | `1` 空闲，`2` 打印中 | |
| `mc_percent` / `percent` | `45` | 进度百分比 |
| `mc_remaining_time` / `remain_time` | `26` | 剩余时间（分钟） |
| `layer_num` / `total_layer_num` | `22` / `126` | 当前层 / 总层数（`3D` 字段里也有一份） |
| `subtask_name` | 模型名称 | 任务名（MakerWorld 模型名） |
| `gcode_file` / `file` | `/data/Metadata/plate_2.gcode` | 正在打印的 gcode |
| `plate_idx` / `plate_id` / `plate_cnt` | `2` / `2` / `2` | 第几块板 / 共几块 |
| `print_type` | `cloud` | 云打印（App / MakerWorld 发起） |
| `job_id` / `task_id` / `subtask_id` / `project_id` / `profile_id` | 数字字符串 | 任务编号 |
| `nozzle_temper` / `nozzle_target_temper` | `255.0` / `255.0` | 喷嘴温度（PETG） |
| `bed_temper` / `bed_target_temper` | `70.0` / `70.0` | 热床温度 |
| `spd_lvl` / `spd_mag` | `2` / `100` | 速度档位 / 百分比 |
| `nozzle_type` / `nozzle_diameter` | `HH01` / `0.4` | 喷嘴型号 / 直径 |

### 2.2 阶段 `stg_cur`

| 值 | 含义（参考 ha-bambulab） | 实测出现的时机 |
|---|---|---|
| `-1` | 不在打印 | 空闲、取消后、打完后 |
| `13` | 归位 | 开始打印后最先出现，约 1 分钟 |
| `11` | 识别打印板 | 归位后，约 7 秒 |
| `4` | **换料（进料）** | 把耗材送进挤出机，约 1.5 分钟；外置料盘没送到时停在这里报 `0x07FFC006` |
| `8` | 挤出校准 | 进料后，约 20 秒 |
| `14` | 清洁喷嘴 | 约 1.5 分钟 |
| `3` | 振动补偿 | 约 25 秒 |
| `54` | 未知（P2S 新增） | 振动补偿后约 6 秒 |
| `1` | 自动调平 | 约 1 分钟 |
| `51` | 未知（P2S 新增） | 调平后、开始打印前约 40 秒 |
| `0` | 正在打印 | |

### 2.3 耗材和挤出机

**`hw_switch_state`**（打印机挤出机前的耗材传感器）

| 值 | 含义 |
|---|---|
| `2` | 挤出机里没有耗材 |
| `3` | 挤出机里有耗材 |

bit0 = 有耗材；bit1 在 P2S 上一直是 1（推测：传感器存在）。

**`device.extruder.info[0]`**（P2S 的新协议字段，比 `hw_switch_state` 信息多）

| 字段 | 实测值 | 说明 |
|---|---|---|
| `id` | `0` | 挤出机编号（P2S 只有一个） |
| `snow` | `65535` / `65280` | **当前料盘**。高 8 位是料盘组，低 8 位是槽位：`0xFFFF`（65535）= 无，`0xFF00`（65280）= 外置料盘（组 255、槽 0）。按这个格式，AMS 第 1 台第 3 槽应该是 `0x0002`（未实测） |
| `spre` | 同上 | 上一个料盘 |
| `star` | 同上 | 目标料盘。进料时先变成目标，进好后 `snow` 才跟着变；退料时先变成 `0xFFFF` |
| `info` | `1032`、`1102` 等 | 状态位，见下表 |
| `temp` | `16711935`、`107` | **打包的喷嘴温度**：高 16 位是目标温度，低 16 位是当前温度。`16711935` = `0x00FF00FF` = 目标 255℃、当前 255℃；`107` = 目标 0、当前 107℃（在降温） |
| `stat` | `197376` 等 | 未知 |
| `hnow` / `hpre` / `htar` | `0` | 推测：当前 / 上一个 / 目标热端编号 |
| `filam_bak` | `[]` | 推测：备用耗材（自动续料） |

`info` 的位（基于 4400 多条记录对照 `hw_switch_state`）：

| 位 | 判断 |
|---|---|
| bit1（`0x002`） | **挤出机里有耗材**，和 `hw_switch_state` 完全一致（只在变化的那一秒有先后）。SEMI-AMS 用的就是这一位 |
| bit3（`0x008`）、bit10（`0x400`） | 一直是 1 |
| bit2（`0x004`） | 含义未知：打印中为 1；耗材离开挤出机（bit1 清零）后约 20 秒才清零 |
| bit5、bit6（`0x020`、`0x040`） | 随加热、换料阶段变化，含义未知 |

实测出现过的值：空闲无料 `1032`（0x408）；打印中 `1102`（0x44E）；其它 `1034 1036 1038 1064 1066 1068 1070 1096 1098 1100 1128 1130 1134`。

**`ams.tray_now` / `tray_tar` / `tray_pre`**（字符串）

`"255"` = 无。**用外置料盘时 P2S 报的是 `"0"`**，和 AMS 第 1 槽一样，分不出来。老资料里外置料盘是 `"254"`，在 P2S 上不成立。所以 P2S 上判断"是不是外置料盘"要用 `extruder.info[].snow`，不能用 `tray_now`。

**`mapping`**：当前任务的耗材映射，格式和 `snow` 一样。实测 `[65535, 65280]`：切片里第 1 种耗材没用，第 2 种耗材 → 外置料盘。打印过程中每条状态都带着它。

**`vir_slot`**：外置料盘的耗材设置（在打印机屏幕、App 或 Studio 里设置的），格式和 AMS 料盘一样：

| 字段 | 实测值 | 说明 |
|---|---|---|
| `id` | `"255"` | 外置料盘 |
| `tray_type` | `PETG` | 耗材类型 |
| `tray_color` / `cols` | `057748FF` / `["057748FF"]` | 颜色 RRGGBBAA（深绿） |
| `tray_info_idx` | `GFG99` | 耗材预设编号（拓竹的耗材 ID） |
| `nozzle_temp_min` / `nozzle_temp_max` | `220` / `270` | 喷嘴温度范围 |
| `tray_diameter` / `total_len` | `1.75` / `330000` | 直径 / 总长度 |
| `remain` / `tray_weight` | `0` / `0` | 外置料盘没有余量信息 |
| `tag_uid` / `tray_uuid` | 全 0 | 外置料盘没有 RFID |

三段记录里 `vir_slot` 一直没变过。改了外置料盘设置后它会不会马上更新，还没测。

**`ams`**（AMS 信息）

| 字段 | 实测值 | 说明 |
|---|---|---|
| `ams[].id` / `info` | `"0"` / `"10002003"` | 第几台 AMS / 状态 |
| `ams[].temp` / `humidity` / `humidity_raw` | `26.4` / `"2"` / `"36"` | 温度、湿度等级、湿度 % |
| `ams[].dry_*` | | 烘干设置 |
| `ams[].tray[]` | 4 个槽 | 每槽的 `tray_type`、`tray_color`、`tray_info_idx`、`remain`、`tag_uid` 等，格式同 `vir_slot` |
| `ams_exist_bits` / `tray_exist_bits` / `tray_is_bbl_bits` | `"1"` / `"f"` / `"f"` | 哪几台 AMS 在、哪几个槽有料、哪几个槽是拓竹耗材 |

实测 AMS 的 4 个槽：PETG 白（GFG03）、PLA 黄、PLA 白、PLA 橙（GFA06）。

**`ams_status`**（`print` 下的顶层字段，不是 `ams` 里面的）

高字节是大状态，低字节是步骤（推测）：

| 值 | 十六进制 | 实测出现的时机 |
|---|---|---|
| `0` | `0x000` | 空闲 |
| `258` | `0x102` | 换料开始（加热），进料和退料都有 |
| `259` | `0x103` | 退料：切断（推测） |
| `260` | `0x104` | 退料：回抽，之后报 `0x07FFC003` 等人拔料 |
| `261` | `0x105` | 进料：**等外置料盘的耗材送进来**（同时报 `0x07FFC006`） |
| `263` | `0x107` | 进料：耗材已到，挤出冲刷（推测），和 `258` 交替几次 |
| `768` | `0x300` | 打印中 |

### 2.4 错误和提示

**`print_error`**（十进制整数，下表写成十六进制）

| 值 | 含义 | 实测情况 |
|---|---|---|
| `0x0300400C` | 打印任务已取消 | 停止打印后 3–5 秒出现，之后被清除变回 0（多数时候能看到 App 发的 `clean_print_error`） |
| `0x07FFC003` | **请从挤出机拔出外置料盘的耗材** | 外置料盘退料的最后一步：打印机切断、回抽完以后报这个并一直等着，耗材离开挤出机后自动清除。SEMI-AMS 收到就马上退料 |
| `0x07FF8003` | **请从挤出机拔出外置料盘的耗材**（HMS 07FF-8003） | 2026-11-06：`0x07FFC003` 报出后一直没人拔，**120 秒后**变成这个，继续等。进料器重启后连上打印机时，读到的也可能已经是它。SEMI-AMS v1.2.3 起同样当作拔出提示（从 C003 变成 8003 不重复触发） |
| `0x03008003` | 打印暂停（推测是 AI 检测到炒面，HMS 0300-8003，未核实） | 2026-11-06：打印中出现，同时 `gcode_state` RUNNING → PAUSE；约 7 分钟后停止打印（`stop`），随后报 `0x0300400C` |
| `0x05008061` | 未知 | 2026-11-06：开始打印后、`stg_cur` 13 → 11 后约 8 秒出现，同时 `gcode_state` RUNNING → PAUSE；在 App 上点继续（`resume`）后恢复打印 |
| `0x07FFC006` | **请把外置料盘的耗材送进挤出机**（推测） | 进料阶段（`stg_cur` 4，`ams_status` 261）耗材迟迟不到时出现；在 App 点"继续"（`ams_control` resume）后清除 |

**`hms`**（`[{"attr":…, "code":…}]`，按拓竹 Wiki 的写法 `attr 高16位-低16位-code 高16位-低16位`）

| HMS | 伴随的 `print_error` | 实测情况 |
|---|---|---|
| `07FF-2000-0002-0004` | `0x07FFC003` | 请拔出外置料盘耗材 |
| `07FF-2000-0002-0002` | `0x07FFC006` | 等外置料盘耗材送入 |
| `0C00-0300-0002-0014` | 无 | 从开始打印到打完一直存在；`0C00` 是摄像头/AI 检测模块，具体含义查拓竹 Wiki |
| `0300-9600-0003-0001` | 无 | 进料阶段短暂出现，含义未知 |

### 2.5 其它字段（目前用不到）

`fan_gear`、`cooling_fan_speed`、`big_fan1_speed`、`big_fan2_speed`、`heatbreak_fan_speed`（风扇）；`lights_report`（灯）；`ipcam`、`xcam`、`xcam_status`（摄像头、AI 检测）；`sdcard`；`upgrade_state`（固件升级）；`net`（网络，**含 IP**）；`wifi_signal`；`online`；`care`（保养计数）；`queue*`（打印队列）；`device.nozzle`（喷嘴型号、磨损）；`device.toolhead`（打印头位置）；`device.ctc`（腔体温度）；`fun`、`fun2`、`cfg`、`aux`、`stat`（功能位，未知）。

---

## 3. 命令回报

打印机收到 App、Studio 或云端发来的命令后，会把命令内容连同结果发到 `report` 主题。它们和 `push_status` 在同一个 `print` 键下，靠 `command` 区分。实测见到的：

| `command` | 什么时候 | 关键字段 |
|---|---|---|
| `project_file` | **开始打印** | 见下文 |
| `gcode_line` | 开始打印前后很多条 | `result`、`return_code`（App 或云端发的内部 gcode，内容不回报） |
| `stop` | 停止打印 | `result` |
| `clean_print_error` | 在 App 上清除错误 | `print_error`（被清除的错误码） |
| `ams_control` | 在 App 上点"继续" | `param: "resume"` |
| `ams_change_filament` | **换料 / 退料** | `ams_id`、`slot_id`、`target`、`soft_temp`、`temper_check`、`errno`。实测在屏幕上退外置料盘：`ams_id 255, slot_id 255, target 255` |

**`project_file`**（开始打印）的字段：

| 字段 | 实测值 | 说明 |
|---|---|---|
| `use_ams` | `true` | 注意：用外置料盘时也是 `true`，不能只看它 |
| `ams_mapping` | `[-1, 255]` | 切片里每种耗材对应的料盘（老格式）：`-1` 没用，`255` 外置料盘 |
| `ams_mapping2` | `[{ams_id:255,slot_id:255},{ams_id:255,slot_id:0}]` | 新格式：`slot_id 255` = 没用；`ams_id 255, slot_id 0` = 外置料盘 |
| `param` | `Metadata/plate_2.gcode` | 3MF 里的哪块板 |
| `url` | `https://…oss-cn-shanghai.aliyuncs.com/makerworld/…/xxx.3mf?X-Amz-…&X-Amz-Expires=3600…` | **打印文件的下载链接**。云打印（MakerWorld）时是带签名的 HTTPS 链接，1 小时内有效。3MF 里的 `Metadata/slice_info.config` 写着这块板用到的每种耗材的类型、颜色、预设编号和用量。从 Studio 局域网直接发的任务，这里是打印机本地路径（未实测） |
| `md5` | | 3MF 的 MD5 |
| `subtask_name`、`design_id`、`model_id`、`profile_id`、`project_id`、`job_id`、`task_id`、`plate_idx` | | 任务信息 |
| `bed_leveling`、`flow_cali`、`vibration_cali`、`layer_inspect`、`timelapse`、`auto_bed_leveling`、`extrude_cali_flag`、`nozzle_offset_cali` | | 打印选项 |
| `dev_id` | `<序列号>` | 打印机序列号 |
| `is_from_mqtt` | `true` | |
| `result` | `SUCCESS` | 命令执行结果；失败时 SEMI-AMS 不送料 |

---

## 4. 实测时间线

### 4.1 外置料盘完整打印（21:27–22:21）

| 时间 | 打印机消息 | SEMI-AMS |
|---|---|---|
| 21:27:00 | `project_file`，`ams_mapping2` 有外置料盘 | 开始送料到满 |
| 21:27:01 | `gcode_state` FAILED → PREPARE | |
| 21:27:05 | → RUNNING | |
| 21:27:12 | `stg_cur` 13（归位）；`info` 1032 → 1064 | |
| 21:27:41 | `hw_switch_state` 2 → 3；`snow` → 65280；`tray_now` 255 → 0；`info` → 1102 | 耗材已到挤出机 |
| 21:28:18 | `stg_cur` 11（识别打印板） | |
| 21:28:25 | `stg_cur` 4（换料）；`star` → 65280；`ams_status` 0 → 258 | |
| 21:28:56–21:29:59 | `ams_status` 258 ↔ 263 交替 | |
| 21:30:03 | `stg_cur` 8；`ams_status` → 768 | |
| 21:30:26–21:34:30 | `stg_cur` 14 → 3 → 54 → 1 → 51 → 0 | |
| 21:34:30–22:17:09 | 打印中 | 补料约 120 次 |
| 22:17:19 | `gcode_state` RUNNING → FINISH；`stg_cur` → -1 | |
| 22:19:25 | `ams_change_filament`（255 / 255 / 255）——在屏幕上点了退料 | 等打印机切断、回抽 |
| 22:19:26 | `ams_status` 0 → 258；`star` → 65535 | |
| 22:20:11 | `ams_status` → 259 | |
| 22:20:15 | `ams_status` → 260 | |
| 22:20:17 | HMS `07FF-2000-0002-0004` | |
| 22:20:18 | `print_error` → `0x07FFC003` | 马上退料 |
| 22:20:21 | `snow` → 65535；`info` → 1036；HMS 清空 | |
| 22:20:22 | `hw_switch_state` 3 → 2；`print_error` → 0；`tray_now` → 255；`ams_status` → 0 | |
| 22:20:56 | | 退到"已退料" |

### 4.2 进料阶段外置料盘没有送到（19:20–19:23）

当时 SEMI-AMS 在反复重启（v1.1.1 内存不足），没有送料。

| 时间 | 打印机消息 |
|---|---|
| 19:20:41 | `project_file`（外置料盘） |
| 19:22:15 | `stg_cur` 4（换料）；`ams_status` 258；`star` → 65280 |
| 19:22:33 | `print_error` `0x07FFC006`；HMS `07FF-2000-0002-0002`；`ams_status` 261——等耗材送进来 |
| 19:23:32 | `ams_control` resume（在 App 上点继续），错误清除 |
| 19:23:34 | `stop` |

### 4.3 中途停止打印（17:40、19:25）

`stop` → 1–4 秒内 `gcode_state` RUNNING → FAILED，`stg_cur` → -1 → 3–5 秒后 `print_error` `0x0300400C` → 之后被清除变回 0。停止后打印机**不会**自己退料，耗材还在挤出机里。

### 4.4 在屏幕上退外置料盘（19:25、22:19）

`ams_change_filament`（`ams_id 255, slot_id 255, target 255`）→ `ams_status` 258 → 259 → 260（约 50 秒，加热、切断、回抽）→ HMS `07FF-2000-0002-0004` + `print_error` `0x07FFC003`，等人拔料 → 耗材离开挤出机后：`hw_switch_state` 3 → 2，`snow` → 65535，`tray_now` → 255，`ams_status` → 0，错误自动清除。

19:26 那次没有进料器帮忙，人先在 App 上清除了错误（`clean_print_error`），过了 1 分钟才把耗材拔出来。

2026-11-06 在屏幕上退料（打印已结束，SEMI-AMS 因为升级重启，不在"打印中"，没有退料）：退料命令 → 73 秒后 `0x07FFC003` → 没人拔，120 秒后变成 `0x07FF8003` → 人按进料器的按钮退料，3 秒后打印机的耗材传感器变成"无"（`hw_switch_state` 3 → 2）。

---

## 5. SEMI-AMS 用到的判断

| 打印机消息 | SEMI-AMS 动作（v1.2.3） |
|---|---|
| `project_file` 成功，`ams_mapping2` 里有外置料盘（或 `use_ams` false） | 开始送料 |
| `gcode_state` 从非打印变成 PREPARE / RUNNING，且当前料盘是外置料盘 | 备用的开始判断 |
| `ams_change_filament` 退外置料盘（`target 255`，`slot_id` 255 或没有） | 等打印机切断、回抽（最长 `mqtt_unload_wait_s`） |
| `print_error` `0x07FFC003`、`0x07FF8003`（H2 的另一个外置料盘 `0x07FEC003`、`0x07FE8003`） | 马上退料（只有"打印中"的那一台退，其它状态只记日志） |
| 挤出机耗材传感器变成"无"（`extruder.info` bit1） | 正在等退料时马上退；空闲时作为备用的退料判断 |

还没用到、以后可能有用的信号：

- `print_error` `0x07FFC006`：打印机在等外置料盘的耗材，可以作为"开始送料"或"重新送料"的信号。
- `mapping` 和 `gcode_state`：SEMI-AMS 中途重启、错过了 `project_file` 时，可以据此知道当前任务正在用外置料盘，接着补料。
- `vir_slot`：外置料盘设成了什么耗材，多台协同时用来选择进料器。
- `project_file.url`：打印文件的下载链接，可以读出任务需要的耗材。
- 进度、剩余时间、任务名：可以显示在点灯 App 上。

---

## 6. 和旧资料不一样的地方（P2S）

- 每秒发一条完整状态，不是只发变化的字段（固件仍然按"没出现的字段保持原值"处理，两种都兼容）。
- 外置料盘在 `ams.tray_now` 里是 `"0"`，不是 `"254"`；要看 `device.extruder.info[].snow`。
- 打印结束或取消后 `gcode_state` 停在 `FINISH` / `FAILED`，没有回到 `IDLE`。
- 从 Bambu Studio 进外置料盘时 `ams_change_filament` 也是 `target 255`，但 `slot_id` 是 0；退料是 `slot_id 255`（出自 ha-bambulab 的消息样本；P2S 实测只见过退料）。
- 用外置料盘打印时 `project_file.use_ams` 也是 `true`。
