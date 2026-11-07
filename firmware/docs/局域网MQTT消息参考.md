# 局域网 MQTT 消息参考

| 项目 | 内容 |
|---|---|
| 适用版本 | v1.3.0 起 |
| 用途 | 把进料器的状态发布到局域网里的 MQTT 服务器（Home Assistant、Node-RED、以后的群控程序），可选接受控制命令 |
| 和拓竹打印机 MQTT 的关系 | 两回事，互不影响。连接打印机的那个见 [拓竹MQTT消息参考](拓竹MQTT消息参考.md) |
| 协议 | MQTT 3.1.1，普通 TCP（默认端口 1883），不加密；可选用户名和密码 |
| 代码 | `src/lanmqtt.cpp`（连接、收发），`src/lanmqtt_proto.h`（topic、设备 ID、控制命令解析，有仿真测试） |

---

## 1. 设置

在设备网页的"局域网 MQTT"卡片里修改，保存后马上按新设置重连。

| 项目 | 参数 | 默认 | 说明 |
|---|---|---|---|
| 启用 | `lan_mqtt_enable` | 关 | |
| 允许远程控制 | `lan_mqtt_control` | 关 | 开了才订阅 `<前缀>control`、执行控制命令。局域网里能连上这个服务器的程序都能发命令，按需要开 |
| IP | `lan_mqtt_ip` | 空 | MQTT 服务器的 IPv4 地址 |
| 端口 | `lan_mqtt_port` | 1883 | 1–65535 |
| topic 前缀 | `lan_mqtt_topic` | `semiams/` | 留空恢复默认，结尾自动补 `/`，最长 32 字节。不能有 `+` `#`、空格、控制字符和 `"` `'` `<` `>` `&` `\`，中文可以 |
| 用户名 | `lan_mqtt_user` | 空 | 留空 = 不登录。Home Assistant 的 Mosquitto 加载项默认要求登录，要在 HA 里建一个用户。可打印 ASCII，最长 32 位，不能有 `"` `'` `<` `>` `&` `\` |
| 密码 | `lan_mqtt_pass` | 空 | 网页上不显示，留空表示不修改；清空用户名时密码也一起清空。可打印 ASCII，最长 64 位 |

卡片上方的"连接"一行显示连接情况：未启用、未填服务器 IP、等点灯初始化完成、连接中、已连接 IP:端口、x 秒后重连（原因）。卡片下方显示本机的状态 topic，例如 `semiams/84f3ebdd2700/state`。

**连接时机**（和连接拓竹打印机的规则一样，避免内存不够）：

- 开机后等点灯初始化完成再连（最多等 90 秒）。
- 空闲内存不到 8000 字节时不连，日志写"空闲内存不足…，暂不连接局域网 MQTT"，60 秒后再看；连着的时候空闲内存低于 3000 字节就先断开。
- 固件升级时断开。
- 只在电机停着的时候发起连接：连不上服务器时 `connect()` 会阻塞最长 2 秒。
- 断开后按 5、10、20、40、60 秒退避重连；用户名或密码错误时 60 秒后再试。

## 2. Topic

`<设备ID>` 是本机 MAC 去掉冒号的小写，例如 MAC `84:f3:eb:dd:27:00` 的设备 ID 是 `84f3ebdd2700`。

| Topic | 方向 | 保留（retain） | 内容 |
|---|---|---|---|
| `<前缀><设备ID>/state` | 设备 → 服务器 | 是 | 状态 JSON（第 3 节） |
| `<前缀><设备ID>/availability` | 设备 → 服务器 | 是 | 在线情况 JSON（第 4 节），offline 也是遗嘱 |
| `<前缀>control` | 服务器 → 设备 | **不要保留** | 控制命令（第 5 节），几台共用，payload 里的 `mac` 等于本机才执行 |

**为什么每台的 state、availability 要用各自的 topic：** 服务器的保留消息按 topic 只存一条。几台共用 `semiams/state` 的话，服务器只留下最后发的那一台，后来才订阅的程序（例如重启后的群控程序、Home Assistant）只看得到一台；遗嘱也一样，一台掉线，服务器就把共用的 availability 写成 offline，另一台明明还在线。

**几台一起收：** 订阅 `semiams/+/state` 和 `semiams/+/availability` 就能收到所有设备。payload 里都带 `ip` 和 `mac`，直接按 payload 区分是哪台，不用去截 topic。

```bash
# 看所有设备的状态和在线情况（-v 同时显示 topic）
mosquitto_sub -h 192.168.5.10 -u 用户名 -P 密码 -v -t 'semiams/+/state' -t 'semiams/+/availability'
```

## 3. 状态 `<前缀><设备ID>/state`

```json
{
  "state": "无耗材",
  "key": "NO_MATERIAL",
  "motor": "停止",
  "duty": 0,
  "in": 0,
  "ready": 0,
  "lack": 0,
  "full": 0,
  "rssi": -77,
  "heap": 12480,
  "uptime": 126,
  "ip": "192.168.5.222",
  "mac": "84:f3:eb:dd:27:00"
}
```

| 字段 | 含义 |
|---|---|
| `state` | 状态的中文名（给人看，以后可能改字） |
| `key` | 状态的固定英文名，**自动化判断用这个**：`NO_MATERIAL` 无耗材、`MATERIAL_LOADING` 耗材吸入中、`MATERIAL_READY` 耗材就绪、`PRINTING` 打印中、`UNLOADING` 退料中、`MATERIAL_RETRACTED` 已退料、`EMERGENCY_STOPPED` 紧急停止 |
| `motor` | 电机：停止 / 正转 / 反转 |
| `duty` | 电机占空比 % |
| `in` / `ready` | 进料口开关 / 就绪开关，1 = 有耗材 |
| `lack` / `full` | EMPTY / FULL 霍尔，1 = 有效 |
| `rssi` | WiFi 信号 dBm |
| `heap` | 空闲内存（字节） |
| `uptime` | 运行时长（秒） |
| `ip` / `mac` | 本机 IP 和 MAC；取不到时为空串 |

**什么时候发：**

- 连上服务器时马上发一份。
- 状态机的状态或电机方向变了就发，两次之间至少隔 500ms。
- 没变化也每 30 秒发一次，当心跳用。只有开关、霍尔变了、状态和电机没变时（例如耗材就绪时霍尔变化），要等这次心跳才发出去。

QoS 0，保留。

## 4. 在线情况 `<前缀><设备ID>/availability`

```json
{"state": "online", "ip": "192.168.5.222", "mac": "84:f3:eb:dd:27:00"}
```

- 连上服务器时发 `online`（保留）。
- 主动断开时（改设置、内存不足、固件升级、超时等）先发 `offline`（保留）再断开。
- 掉电、断网、崩溃时，服务器按遗嘱代发 `offline`（遗嘱内容在连接时就定好了，同样带 ip 和 mac）。
- 保活：每 30 秒一次 PINGREQ，keepalive 60 秒；90 秒收不到服务器任何数据就认为连接已断，重新连接。

## 5. 控制 `<前缀>control`

要先在网页上勾选"允许远程控制"，否则不订阅这个 topic。订阅用 QoS 1。

```json
{"mac": "84:f3:eb:dd:27:00", "start": "on"}
```

- **`mac`**：必填，等于本机 MAC 才执行；不区分大小写，冒号、横线可有可无（`84F3EBDD2700` 也行）。几台共用这个 topic，发给别的设备的命令不记日志。可以带 `ip` 等其它字段，固件忽略。
- **动作**：下表里的一个。一条消息只执行第一个动作。
- **值**：`"on"`/`"off"`、`true`/`false`、`1`/`0`（字符串或数字都行）。
- **不要用保留（retain）发布。** 服务器上的保留控制消息会在每次重连、订阅时补发一遍；固件不执行它，日志写"服务器上有一条保留的控制消息，不执行"。要清掉它，向 `<前缀>control` 发一条空的保留消息（`mosquitto_pub -r -n -t semiams/control`）。

| 动作 | on | off |
|---|---|---|
| `start` | 开始打印：耗材就绪时进入打印中，已退料时先吸料再打印，吸入中时吸完直接打印；已经在打印中或退料中则不动 | 结束打印：只有打印中才开始退料，其它状态不动 |
| `emg` | 紧急停止（已经是紧急停止则不动） | 解除紧急停止，回到原来的状态（不在紧急停止则不动） |
| `jogf` | 正转 2 秒（补料速度），只在无耗材、耗材就绪、已退料时可用 | 不动 |
| `jogr` | 反转 2 秒，条件同上 | 不动 |
| `restart` | 重启 | 不动 |

`start`、`emg` 按 on / off 的含义执行，重复发同一条不会来回切换，这一点和网页、点灯上的按钮（按一下切换一次）不同。

```bash
# 开始打印
mosquitto_pub -h 192.168.5.10 -u 用户名 -P 密码 -t semiams/control -m '{"mac":"84:f3:eb:dd:27:00","start":"on"}'
# 紧急停止
mosquitto_pub -h 192.168.5.10 -u 用户名 -P 密码 -t semiams/control -m '{"mac":"84:f3:eb:dd:27:00","emg":true}'
```

收到命令后，日志写"局域网 MQTT：开始打印"之类的记录；被忽略的原因也会写（不是有效的 JSON、没有 mac、没有可识别的动作、值不认识、状态不对）。

## 6. Home Assistant 示例

下面按 HA 的 MQTT 文档写，**还没在 HA 上实测**，设备 ID 换成自己的：

```yaml
mqtt:
  sensor:
    - name: "SEMI-AMS 状态"
      unique_id: semiams_84f3ebdd2700_state
      state_topic: "semiams/84f3ebdd2700/state"
      value_template: "{{ value_json.state }}"
      availability_topic: "semiams/84f3ebdd2700/availability"
      availability_template: "{{ value_json.state }}"
      payload_available: "online"
      payload_not_available: "offline"
  button:
    - name: "SEMI-AMS 紧急停止"
      unique_id: semiams_84f3ebdd2700_emg
      command_topic: "semiams/control"
      payload_press: '{"mac":"84:f3:eb:dd:27:00","emg":"on"}'
```

## 7. 以后做群控时

- 订阅 `<前缀>+/state` 和 `<前缀>+/availability`，以 payload 里的 `mac` 为键维护每台设备的状态；`key` 字段做判断。
- 发命令用共用的 `<前缀>control`，payload 里写目标设备的 `mac`。
- 拓竹打印机的 MQTT 以后可能移到主控上做（见 [多台SEMI-AMS协同方案](多台SEMI-AMS协同方案.md)）。建议到时候固件里仍保留现在的打印机逻辑，主控不在线时还能单台使用。
