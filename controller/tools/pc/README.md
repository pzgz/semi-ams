# pc — PC 端取数脚本

把拓竹打印机的状态搬到 SEMI-AMS 控制端（C3）的 PC 侧工具，位于 `controller/tools/pc/`。
**全部零第三方依赖**，只用 Python 标准库（3.8+）。

> 同级的 `controller/tools/mqtt_*.py` 是另一批东西：C3 侧 broker 的协议自测脚本，跟本目录无关。

```
打印机 192.168.1.50:8883 (TLS)
        │
        ├─ 本地直连（默认）────┐
        │                      ├─► bambu_bridge.py ─► C3 192.168.1.100:1883
        └─ 拓竹云 MQTT（回落）─┘     semiams/printer        （明文，无 TLS）
                                                              │
                                                              ▼
                                                    C3 /api/state → 网页卡片
```

C3 侧不碰 TLS、不联网，只多一个 MQTT 报文入口。协议细节见
[`../../docs/printer-mqtt.md`](../../docs/printer-mqtt.md)。

---

## 两个文件分别是什么

| 文件 | 角色 | 要单独跑吗 |
|---|---|---|
| **`bambu_bridge.py`** | 桥接主程序：连打印机 → 压缩字段 → 发到 C3 | ✅ 常驻跑的就是它 |
| `bambu_cloud.py` | 底层库：手写的 MQTT 3.1.1 客户端 + 拓竹云 API 客户端；同时也是调试用 CLI | ❌ 被 `import`，不跑也能工作 |

依赖是单向的，`bambu_bridge.py` 第 39 行直接从同目录导入：

```python
from bambu_cloud import Cloud, CloudError, MiniMqtt, load_token
```

所以**两个文件必须在同一目录**，拆开就 `ModuleNotFoundError`。

---

## 快速开始

```bash
# 一条命令跑起来（--broker 必填，topic 有默认值）
python3 controller/tools/pc/bambu_bridge.py \
    --broker 192.168.1.100:1883 \
    --source local \
    --ip 192.168.1.50 \
    --user semiams --password semiams
```

`--broker` 是**必填**的：C3 地址跟具体环境绑定，藏在默认值里容易把数据发到错的机器。省略端口时按 1883 处理（`--broker 192.168.1.100` 也可以）。

`--user/--password` 是 **C3 broker 的账号**，不是拓竹账号；示例里的 `semiams/semiams` 只是占位，请换成你在 C3 上设的用户名和密码。C3 没设账号时可以不填（匿名）。

跑起来后打开 `http://192.168.1.100/` 就能看到打印机卡片。

---

## bambu_bridge.py

### 参数

| 参数 | 默认 | 说明 |
|---|---|---|
| `--broker` | **必填** | C3 地址 `IP:端口`，如 `192.168.1.100:1883`；省略端口按 1883。仅 `--dry-run` 时可省 |
| `--topic` | `semiams/printer` | 发布主题，**必须以 `/printer` 结尾** |
| `--user` / `--password` | 空 | C3 broker 账号，空=匿名 |
| `--source` | `auto` | `auto` 本地优先失败回落；`local` 只本地；`cloud` 只云端 |
| `--ip` | 空 | 打印机 IP，本地模式必填 |
| `--sn` | 自动 | 序列号，不给就读 8883 自签证书的 Subject CN |
| `--code` | 自动 | 访问码，不给就读 Bambu Studio 配置 |
| `--model` | 自动 | 机型，不给就发 `get_version` 读 `product_name` |
| `--region` | 空 | `cn` / `intl`，云端账号区域 |
| `--interval` | `5` | 发布间隔秒；状态有变化会立刻发，不等间隔 |
| `--once` | — | 发一条就退出（验证用） |
| `--cmd` | — | `load` / `unload` / `pause` / `resume` / `stop`，只发这一条命令就退出（调试协议用，不需要 C3） |
| `--dry-run` | — | 只打印 payload，不发给 C3，也不真发给打印机 |
| `-v` | — | 每次发布打印完整 JSON |

### 常用组合

```bash
# 只验证取数，不碰 C3（这种模式不要求 --broker）
python3 controller/tools/pc/bambu_bridge.py --source local --ip 192.168.1.50 --dry-run -v

# 走云端（打印机不在同一网段时）
python3 controller/tools/pc/bambu_bridge.py --broker 192.168.1.100:1883 --source cloud --region cn

# 发到别的 C3 / 别的主题
python3 controller/tools/pc/bambu_bridge.py --broker 192.168.1.101:1883 --topic myams/printer
```

### 网页上的控制按钮

常驻运行时桥接会额外订阅 `semiams/printer/cmd`，C3 网页上的按钮就是往这里发
`{"c":"pause"}` / `"resume"` / `"stop"` / `"load"` / `"unload"`。进退料翻成拓竹的
`ams_change_filament`，打印控制翻成 `pause` / `resume` / `stop`：

```json
// load → slot_id/target = 254（外挂），unload → 255（退掉当前那一路）
{"print":{"command":"ams_change_filament","ams_id":0,"slot_id":254,"target":254,
          "curr_temp":-1,"tar_temp":-1,"soft_temp":0}}
// 暂停 / 继续 / 停止：共用同一个形状
{"print":{"command":"pause","param":""}}
{"print":{"command":"resume","param":""}}
{"print":{"command":"stop","param":""}}
```

只认这五个词，其它一律丢弃。时机检查两边都做（C3 和桥接各拦一次），
每个动作只有一个合法状态：

| 动作 | 要求 `gcode_state` |
|---|---|
| `pause` | `RUNNING` |
| `resume` | `PAUSE` |
| `stop` | `RUNNING` / `PAUSE` / `PREPARE` |
| `load` / `unload` | 空闲（不能是上面三个） |

**注意**：这是 qos0，打印机忙推状态时可能漏收，所以桥接只保证「已下发」，
真实结果看 `gcode_state`、`tray_now` 和喷嘴温度。想脱离 C3 单测协议：

```bash
python3 controller/tools/pc/bambu_bridge.py --source local --ip 192.168.1.50 --cmd unload
# 只打印要发的报文、不真发给打印机：
python3 controller/tools/pc/bambu_bridge.py --source local --ip 192.168.1.50 --cmd stop --dry-run
```

### 控制命令被拒：先开「开发者模式」

打印机绑着云账号时，新固件会给每条 `print` 命令**验签名**，密钥在 Bambu Studio
的私有网络插件里，第三方签不出来。表现是命令发出去了、网页没反应，拓竹 App 弹一条：

> HMS_0500-0500-0001-0007: MQTT 命令校验失败，请更新 Studio 或 Handy

桥接的日志里同时会出现一行「打印机拒了上一条命令」，网页顶栏显示
「命令被拒 · 需开发者模式」，控制区顶上多一条黄色提示条，五个按钮（进退料 +
暂停/继续/停止）全部置灰、悬停提示「需开发者模式」。只读的 `report` 不验签，
所以监控一直是好的，失效的只有这五个动作。

解法是在打印机上把**局域网模式**和**开发者模式**都打开（A 系列：设置 → 局域网模式
→ 开发者模式；X / P 系列在 WLAN 页里），打开后固件就不再验签。代价是打印机脱离
云端，拓竹 App 的远程控制用不了，同网段内 Studio / Handy / 本项目都仍能直连。
详见 `controller/docs/printer-mqtt.md` §2.4。

### 访问码从哪来

本地模式的码有两条来源，按顺序试：

1. `--code` 参数
2. Bambu Studio 配置文件里的 `access_code` 字段（只读，不改原文件）

配置文件路径按平台自动探测：

| 平台 | 路径 |
|---|---|
| macOS | `~/Library/Application Support/BambuStudio/BambuStudio.conf` |
| Windows | `%APPDATA%\BambuStudio\BambuStudio.conf` |
| Linux | `~/.config/BambuStudio/BambuStudio.conf` |

**注意**：文件末尾跟了一行 `# MD5 checksum ...`，不是合法 JSON，所以代码用 `raw_decode` 只解析第一个对象，别直接 `json.load`。

如果两条都拿不到，`--source auto` 会回落到云端 MQTT —— 那条路不需要访问码，只需要 token。

### 打印机断开怎么办

内置自动重连，退避 1s → 2s → 4s → … 上限 30s，连上后复位。

心跳：`keepalive=60`，空闲 25 秒补一次 `PINGREQ`。**这条不能省** —— 早期版本没发心跳，跑 30 分钟左右必然被打印机单方面 RST（`ConnectionResetError: [Errno 54]`）。

---

## bambu_cloud.py

三个子命令，默认 `status`。

| 子命令 | 作用 |
|---|---|
| `login` | 登录并把 token 存到 `~/.bambu_cloud.json`（权限 0600，约 90 天） |
| `devices` | 列账号下已绑定的打印机：昵称、型号、SN、在线、任务状态、访问码（脱敏） |
| `status` | 连云 MQTT 读一次状态 |

### 参数

| 参数 | 说明 |
|---|---|
| `--region` | `cn`（默认）/ `intl` |
| `--code` | 用验证码登录，不走密码 |
| `--sn` | 指定序列号，默认取设备列表第一台 |
| `--watch N` | 持续读取 N 秒（每 5 秒一次） |
| `--json` | 输出机器可读 JSON |
| `--raw` | 打印原始 report 报文（调试解析用） |
| `--relogin` | 忽略缓存 token，强制重新登录 |

### 登录

```bash
python3 controller/tools/pc/bambu_cloud.py login
# 账号也可以走环境变量，避免交互
BAMBU_ACCOUNT=你的手机号 BAMBU_PASSWORD=xxx python3 controller/tools/pc/bambu_cloud.py login
```

密码只在内存里用，**不落盘**；落盘的只有 token。

⚠️ **短信验证码接口目前被极验人机验证拦截**（HTTP 418 + `captchaId`），程序绕不过去。所以：

- 手机号账号 → 只能用密码登录
- 想用验证码 → 换成邮箱账号（`sendemail/code` 接口正常）

### 其他用法

```bash
python3 controller/tools/pc/bambu_cloud.py devices            # 看有几台、叫什么、在不在线
python3 controller/tools/pc/bambu_cloud.py devices --json     # 喂给别的程序
python3 controller/tools/pc/bambu_cloud.py status --watch 60  # 连续读 60 秒
python3 controller/tools/pc/bambu_cloud.py status --raw       # 看打印机原始报文
```

`devices` 输出里的访问码是**打码的**（`f2****9d`），完整码不会打到终端。

---

## 调试顺序

出问题时按这个顺序排，能快速定位是哪一段断了：

```bash
# 1. 打印机 8883 通不通（TLS 握手）
python3 -c "import socket; socket.create_connection(('192.168.1.50',8883),timeout=3); print('8883 OK')"

# 2. 能不能读到状态（不经过 C3）
python3 controller/tools/pc/bambu_bridge.py --source local --ip 192.168.1.50 --dry-run -v

# 3. C3 broker 通不通
python3 -c "import socket; socket.create_connection(('192.168.1.100',1883),timeout=3); print('1883 OK')"

# 4. C3 到底收到没有
curl -s http://192.168.1.100/api/state | python3 -c "import sys,json;d=json.load(sys.stdin);print(d['mqtt'],d['printer']['valid'],d['printer']['reports'])"
```

第 4 步 `mqtt.in` 在涨但 `printer.valid` 是 `false` → 主题没对上（检查是不是 `/printer` 结尾）；
`mqtt.in` 不涨 → 桥接根本没发出去，回到第 2 步。

---

## 常见坑

| 现象 | 原因 | 处理 |
|---|---|---|
| C3 收到数据但网页全白 | JSON 不合法 | 大概率是 C3 侧 `snprintf` 截断，不是 PC 端问题 |
| 中文变 `\u9001\u6599` 字面量 | `json.dumps` 默认转义 | 必须 `ensure_ascii=False`，代码已处理 |
| 主题发成 `semiams/print` | C3 靠 `endsWith("/printer")` 匹配 | 会被当成送料器报文丢进 `/state` 分支 |
| 跑半小时崩 `Errno 54` | 没发 MQTT 心跳 | 已修：空闲 25s 补 `PINGREQ` |
| 型号永远读不到 | `get_version` 应答形状记错 | `module` 直接挂在 `info` 下，没有嵌套 `get_version` 键 |
| 界面永远显示「未取料」，AMS/外挂不区分 | `tray_now` 错读成 `ams.ams[0]["tray_now"]`，单元里没这个键 | 它在 `print.ams.tray_now`，与 `ams.ams` 数组同级；改完要重启桥接 |
| 关掉「局域网模式」后连不上 | 访问码会重新生成 | 让 Bambu Studio 打开一次（配置会同步新码），或走云端 |
| 拓竹 App 弹 `HMS_0500-0500-0001-0007`（MQTT 命令校验失败），网页点按钮没反应 | 固件给 `print` 命令验签名，第三方没有私钥 | 打印机上开「局域网模式 + 开发者模式」，见上文 |
| 短信登录 418 | 极验人机验证 | 用密码登录，或换邮箱账号 |

---

## 相关文件

| 文件 | 内容 |
|---|---|
| `bambu_bridge.py` | 桥接主程序 |
| `bambu_cloud.py` | MQTT 客户端 + 云 API 库 + 调试 CLI |
| `../../docs/printer-mqtt.md` | C3 侧主题约定、字段表、解析规则 |
| `../../src/prstate.cpp` | C3 侧报文解析实现 |
| `../../src/webpage.h` | 网页打印机面板渲染 |
