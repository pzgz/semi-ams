# 拓竹打印监控：MQTT 主题与数据字段

本文档描述 SEMI-AMS 控制端（C3，`SEMI-AMS-CTRL`）如何接收拓竹打印机状态。

---

## 1. 总体链路

```
拓竹打印机 192.168.1.50:8883 (TLS, 自签)
        │  ① device/<SN>/report  (订阅)
        │  ② device/<SN>/request (发布 pushall / get_version)
        ▼
PC 端 tools/pc/bambu_bridge.py        ← 唯一需要做 TLS 的地方
        │  ③ semiams/printer  (明文 MQTT，qos0)
        ▼
C3 broker :1883 (SEMI-AMS-CTRL)
        │  ④ broker.cpp 按主题后缀分发 → printerOnState()
        ▼
C3 内存 PrinterState (prstate.cpp)
        │  ⑤ GET /api/state → printer{...}
        ▼
网页 webpage.h → 打印机面板
```

设计原则：**C3 不做 TLS、不连打印机、不存访问码**。加解密和凭据全部留在 PC 端，
C3 只做一件事——把一条扁平 JSON 报文拆进结构体。这样 C3 固件不含 mbedtls，
Flash 占用从 ~76% 降到 ~64%。

`src/broker.cpp` 中只有一个入口判断：

```cpp
if (endsWith(topic, "/state"))             devicesOnState(gPayload);
else if (endsWith(topic, "/availability")) devicesOnAvailability(gPayload);
else if (endsWith(topic, "/printer"))      printerOnState(gPayload);   // ← 打印机
```

---

## 2. 主题

### 2.1 打印机侧（PC 端 ↔ 打印机，需要 TLS）

| 方向 | 主题 | QoS | 说明 |
|---|---|---|---|
| 订阅 | `device/<SN>/report` | 0 | 打印机上报状态；**增量报文**，只带变化字段，收到需深合并 |
| 发布 | `device/<SN>/request` | 0 | 请求全量状态 / 版本信息 |

`<SN>` 为打印机序列号，例如 `03900D5A2591992`。**不做通配订阅**：一台桥接进程只跟一台打印机。

**连接参数**

| 模式 | 主机 | 端口 | 用户名 | 密码 | TLS |
|---|---|---|---|---|---|
| 本地（默认） | 打印机 IP | 8883 | `bblp` | 8 位访问码 | 自签，需跳过校验 |
| 云端（回退） | `cn.mqtt.bambulab.com` | 8883 | `u_<uid>` | accessToken (JWT) | 正规 CA |

**两条 request 报文**

请求全量状态（`device/<SN>/request`）：

```json
{"pushing":{"sequence_id":"0","command":"pushall","version":1,"push_target":1}}
```

请求版本信息（`device/<SN>/request`）：

```json
{"info":{"sequence_id":"1","command":"get_version"}}
```

`pushall` 用于拿一次全量快照（桥接启动时、以及空闲 45 秒补一次）。
`get_version` 应答形状是 **`module` 直接挂在 `info` 下**，没有嵌套的 `get_version` 键：

```json
{"info":{"command":"get_version","module":[{"product_name":"Bambu Lab A1", "...": "..."}]}}
```

`product_name` 去掉 `"Bambu Lab "` 前缀后即机型（`A1` / `X1C` / `P1S` …）。

**序列号怎么来**：打印机自签证书的 Subject CN 就是 SN。桥接在 TLS 握手后读证书，
不用手输。访问码优先级：`--code` 参数 → Bambu Studio 配置 `access_code` → 云端 `bind` 接口。

### 2.2 控制端侧（PC 端 → C3，明文本地网）

| 方向 | 主题 | QoS | Retain | 说明 |
|---|---|---|---|---|
| 发布 | `semiams/printer`（可改） | 0 | 否 | 打印机状态，每台打印机一条主题 |

**约束**

- 主题必须以 **`/printer`** 结尾。C3 靠后缀匹配，不以 `/printer` 结尾的报文
  既不会进打印机状态，也可能被 `/state` 分支当成送料器报文吃掉。
- 前缀随意（默认 `semiams/`），多台打印机可各用一条主题，例如
  `semiams/printer/a1`、`semiams/printer/x1c`。
  **但这些主题必须同样以 `/printer` 结尾才生效**；若要多机并行，需要改固件分发逻辑。
- C3 broker 若设置了账号，发布方必须带上；当前现场为 `root` / `root`。
- C3 broker 会把非空 payload 存一份 retained（`setRetained`），但存在 RAM 里，
  重启即失效；桥接的下一条报文（状态变化或 5 秒兜底）会立刻补齐。

---

## 3. 报文格式（PC 端 → C3）

一条**扁平 JSON 对象**，无嵌套对象、无数组（`jsonmac` 只认扁平键）。典型 330–340 字节，
上限 **768 字节**（`config.h` 的 `MQTT_BODY_MAX`）。

```json
{"st":"FINISH","pct":100,"ly":152,"tly":152,"rem":0,"f":"送料器配件改进",
 "nz":17,"nzt":0,"bd":16,"bdt":0,"ch":5,"wf":-29,"spd":2,"er":0,"hn":0,"h1":"",
 "ams":"PETG:C52C18:-1|PETG:FFFFFF:-1|PETG:057748:-1|PETG:898989:-1",
 "aid":"0","amsh":2,"amst":20,"tn":255,"vt":"PETG:F72323",
 "sn":"03900D5A2591992","src":"local","md":"A1"}
```

**编码要求**：必须发 UTF-8 原文，不能发 `\uXXXX` 转义。C3 的 `jsonmac` 按字节抓字符串，
收到转义序列只会得到字面量 `\u9001` 而不会还原成汉字。Python 侧即
`json.dumps(..., ensure_ascii=False)`。

### 3.1 字段表

#### 状态与进度

| 键 | 类型 | 单位/取值 | 固件字段 | 说明 |
|---|---|---|---|---|
| `st` | string | `IDLE` `PREPARE` `SLICING` `RUNNING` `PAUSE` `FINISH` `FAILED` … | `gcode_state` | 机台状态机 |
| `pct` | int | % | `mc_percent` | 打印进度 |
| `ly` | int | 层 | `layer_num` | 当前层 |
| `tly` | int | 层 | `total_layer_num` | 总层数 |
| `rem` | int | **分钟** | `mc_remaining_time` | 剩余时间；非打印态通常是 0 |
| `f` | string | ≤40 字符 | `subtask_name`，回退 `gcode_file` | 文件名，UTF-8 |

#### 温度与网络

| 键 | 类型 | 单位 | 固件字段 | 说明 |
|---|---|---|---|---|
| `nz` / `nzt` | int | °C | `nozzle_temper` / `nozzle_target_temper` | 喷嘴当前 / 目标 |
| `bd` / `bdt` | int | °C | `bed_temper` / `bed_target_temper` | 热床当前 / 目标 |
| `ch` | int | °C | `chamber_temper` | 腔体温度；A1 系列无腔温传感器，恒 0 |
| `wf` | int | dBm | `wifi_signal` | 由字符串 `"-29dBm"` 解析成整数 |
| `spd` | int | 1–4 | `spd_lvl` | 速度档 |

#### 告警

| 键 | 类型 | 说明 |
|---|---|---|
| `er` | int | `print_error`，0 = 无错误 |
| `hn` | int | HMS 告警条数 |
| `h1` | string | 第一条 HMS，格式 `"ATTR-CODE"` 四位大写十六进制，如 `"0C00-0102"`；无告警为空串 |

C3 只存第一条 HMS（`hms1[16]`），完整列表不进控制端。

#### AMS

| 键 | 类型 | 取值 | 说明 |
|---|---|---|---|
| `ams` | string | `材料:颜色:余量\|…` | 最多 4 槽，见 3.2 |
| `aid` | string | `"0"` / `"1"` … | AMS 单元号 |
| `amsh` | int | 1–5，`-1` | 湿度**档位**，不是百分比（固件给的就是档位） |
| `amst` | int | °C，`-1` | AMS 腔温；AMS Lite / A1 mini 不报，为 `-1` |
| `tn` | int | `255` `254` `0`–`3` | `tray_now`：255 空闲 / 254 用外挂料盘 / 0–3 正在取料的槽位 |
| `vt` | string | `材料:颜色`，空串 | 外挂料盘，如 `"PETG:F72323"`；无则空串 |

`amsh` / `amst` / `tn` 缺字段时的默认值分别是 `-1` / `-1` / **`255`**。
`tn` 必须默认 255 —— 结构体 `memset` 出来的 0 会被误判成「正在使用槽位 1」。

#### 设备身份（可选，顶栏用）

| 键 | 类型 | 来源 | 说明 |
|---|---|---|---|
| `sn` | string | 打印机证书 Subject CN | 序列号 |
| `md` | string | `info.get_version` → `module[0].product_name` | 机型，如 `A1` |
| `src` | string | 桥接自身 | `local`（直连 8883）/ `cloud`（云 MQTT） |

空值字段会被桥接直接省略（不发送空串），C3 侧缺失即留空，网页顶栏用
`[md, sn].filter(Boolean).join(' · ')` 渲染，所以空段不会留下多余的分隔符。

> 早期版本还有个 `nm`（打印机昵称）。它只能从拓竹云 `bind` 接口取，本地直连
> 根本拿不到，等于永远空着，已连同固件字段、网页渲染、`--name` 参数一起删掉。

### 3.2 `ams` 扁平串格式

```
PETG:C52C18:-1|PETG:FFFFFF:-1|PETG:057748:-1|PETG:898989:-1
└─┬─┘ └─┬──┘ └┬┘
 材料   颜色  余量
```

- 槽位之间用 `|` 分隔，从左到右对应槽位 1、2、3、4
- 每段固定三段 `材料:颜色:余量`
- 材料截断到 6 字符，颜色为 6 位大写十六进制 RGB（无 `#`）
- 余量：`-1` = 未知。**非 RFID 卷固件恒报 100，不可信**，桥接因此统一转成 `-1`
- `tray_type` 为空的槽位（空槽）不出现，所以装 2 卷时串里只有 2 段
- 单段长度必须 < 24 字节，解析器 `parseTrays()` 会丢弃超长的段；最多取 4 段

为什么不用数组：`jsonmac` 只认扁平键，给 C3 加个数组解析器不如在 PC 端拼字符串省事，
报文也更短。

---

## 4. C3 侧解析结果

`src/prstate.h` 的 `PrinterState`，字段与报文键一一对应：

```cpp
struct PrinterTray {
  char type[8];   // PETG
  char color[8];  // C52C18
  int  remain;    // -1 = 未知
};

struct PrinterState {
  bool     valid;      // 收到过至少一条报文
  uint32_t lastMs;     // 最后一条的 millis
  uint32_t reports;    // 累计条数

  char name[28], model[20], sn[20], src[8];   // 顶栏身份

  char st[12];                                // gcode_state
  int  pct, layer, total, remainMin;
  char file[48];                              // UTF-8

  int  nozzle, nozzleTarget, bed, bedTarget, chamber, wifi, spd, err;

  int  hmsCount;
  char hms1[16];

  char amsId[4];
  int  amsHum, amsTemp, trayNow;

  int  trayCount; PrinterTray tray[4];
  bool hasVt; char vtType[8], vtColor[8];
};
```

**并发**：`printerOnState()` 在 broker 任务里写，网页任务读，全部走
`portENTER_CRITICAL(&gMux)`；网页只拿 `printerCopy()` 的快照。

**字符串截断**：`copyStr()` 做 UTF-8 边界回退，宁可少一个汉字也不切半个多字节字符，
否则 `/api/state` 会输出非法 JSON，网页 `JSON.parse` 整页白屏。

**超时不自动清空**：桥接断线后 `valid` 保持 `true`，只是 `age` 一直涨。
是否算「掉线」由消费方判断，网页的阈值是 **age > 20 秒** 视为过期（顶栏 chip 转黄、圆点变灰）。

---

## 5. `/api/state` 输出

```json
{"printer": {
  "valid": true, "age": 3, "reports": 47,
  "st": "FINISH", "pct": 100, "ly": 152, "tly": 152, "rem": 0,
  "nz": 17, "nzt": 0, "bd": 16, "bdt": 0, "ch": 5,
  "wf": -29, "spd": 2, "er": 0, "hn": 0, "h1": "",
  "md": "A1", "sn": "03900D5A2591992", "src": "local",
  "amsid": "0", "amsh": 2, "amst": 20, "tn": 255,
  "file": "送料器配件改进",
  "trays": [
    {"type": "PETG", "color": "C52C18", "remain": -1},
    {"type": "PETG", "color": "FFFFFF", "remain": -1},
    {"type": "PETG", "color": "057748", "remain": -1},
    {"type": "PETG", "color": "898989", "remain": -1}
  ],
  "vt": {"type": "PETG", "color": "F72323"}
}}
```

| 字段 | 说明 |
|---|---|
| `age` | 距最后一条报文的秒数；**从没收到过时为 `-1`** |
| `reports` | 累计收到的报文条数（C3 重启归零） |
| `md` / `sn` / `src` | 同报文，只是键名带 `printer` 前缀语义；都可能为空串 |
| `trays` | 数组，长度 = `trayCount`（0–4），非固定四格 |
| `vt` | `null` 表示没有外挂料盘 |

未接入时整块为 `"printer":{"valid":false,"age":-1,...,"trays":[],"vt":null}`。

页面对 `st` 的中文映射在 `webpage.h` 的 `PR_ZH`：

| `st` | 显示 | chip 配色 |
|---|---|---|
| `IDLE` | 空闲 | 灰 |
| `PREPARE` | 准备中 | 灰 |
| `SLICING` | 切片中 | 灰 |
| `RUNNING` | 打印中 | 蓝 |
| `PAUSE` | 已暂停 | 橙 |
| `FINISH` | 已完成 | 绿 |
| `FAILED` | 已失败 | 红 |

---

## 6. 发送策略（PC 端）

`tools/pc/bambu_bridge.py`：

- **变化即推**：签名 `(st, pct, ly, er, hn, ams, md, sn)` 任意一项变化立即发布
- **定时兜底**：`--interval`（默认 5 秒）没变化也补一条，防止消费端长时间无报文
- **推送触发**：桥接启动时发一次 `pushall`；之后空闲超过 45 秒再补一次全量
- **发布失败**（C3 重启 / 断网）：捕获异常 → 重连 broker → 下一条报文恢复，不退出
- 报文只发 `gcode_state` 已有值的情况，首包未到时跳过

---

## 7. 自助验证

```bash
# 1. 看 C3 收到的状态
curl -s http://192.168.1.100/api/state | python3 -c \
  "import sys,json;print(json.load(sys.stdin)['printer'])"

# 2. 旁听主题（需 mosquitto-clients）
mosquitto_sub -h 192.168.1.100 -u semiams -P semiams -t 'semiams/printer' -v

# 3. 手工灌一条假报文，验证固件解析与网页渲染
mosquitto_pub -h 192.168.1.100 -u semiams -P semiams -t 'semiams/printer' -m \
'{"st":"RUNNING","pct":42,"ly":26,"tly":94,"rem":22,"f":"测试件",
  "nz":245,"nzt":250,"bd":60,"bdt":60,"ch":0,"wf":-58,"spd":2,"er":0,"hn":0,"h1":"",
  "ams":"PLA:C52C18:-1|PETG:FFFFFF:-1","aid":"0","amsh":2,"amst":-1,"tn":1,
  "vt":"","sn":"TEST","src":"local","md":"A1"}'

# 4. 桥接 dry-run，不发给 C3，只看报文
python3 controller/tools/pc/bambu_bridge.py --source local --ip 192.168.1.50 --dry-run -v
```

手工灌的报文会一直保留到桥接下一条真实报文覆盖它；想彻底清掉，
重启 C3 或发一条 `"st":""` 让状态回到初始解析结果。

---

## 8. 已知限制与踩坑

| 现象 | 原因 | 处理 |
|---|---|---|
| 网页整页白屏、控制台 `JSON.parse` 报错 | 字符串截断切成半个汉字，产生非法 JSON | `copyStr()` 已做 UTF-8 边界回退；新增字符串字段务必用它 |
| `/api/state` 里出现 `"hn":,` | `snprintf` 缓冲不够，把最后一个数值截掉了 | 数值段单独用 `pbuf[192]`，不要复用 `buf[128]` |
| 面板显示「AMS 使用槽位 1」但其实空闲 | `tn` 缺省用了 0 | 默认值必须是 255 |
| 中文变成 `\u9001\u6599` 字面量 | 发布方用了 `ensure_ascii=True` | 必须 `ensure_ascii=False` + UTF-8 |
| 报文被当成送料器状态 | 主题没以 `/printer` 结尾 | 主题必须以 `/printer` 结尾 |
| 型号永远空 | 按 `info["get_version"]["module"]` 取 | 实际是 `info["module"]` |
| 桥接停掉后状态一直「在线」不动 | C3 不清空，只让 `age` 涨 | 消费方按 `age > 20` 判过期 |

---

## 9. 相关文件

| 文件 | 作用 |
|---|---|
| `controller/src/prstate.h/.cpp` | 报文解析、状态存储、并发保护 |
| `controller/src/broker.cpp` | 主题分发（`endsWith(topic, "/printer")`） |
| `controller/src/webui.cpp` | `/api/state` 里的 `printer` 块 |
| `controller/src/webpage.h` | 打印机面板、`PR_ZH` 状态映射、`renderPrinter()` |
| `controller/include/config.h` | `MQTT_BODY_MAX = 768`、固件版本 |
| `tools/pc/bambu_bridge.py` | PC 端桥接：连打印机 + 压成扁平 JSON + 发 C3 |
| `tools/pc/bambu_cloud.py` | 云端登录、设备列表、访问码获取 |
