# 主控 / 控制端（ESP32-C3）

多台 SEMI-AMS 进料器的局域网群控端（接几台显示几行，不写死行数）。**这块 ESP32-C3 本身就是 MQTT 服务端**，
进料器直接连它（不需要 Mosquitto、Home Assistant 或别的常开电脑），
状态排成一张表显示在网页上，点按钮单独控制某一台。

原理只有一条：进料器按 [局域网 MQTT 协议](../firmware/docs/局域网MQTT消息参考.md) 连上来。

| 方向 | Topic | 用途 |
|---|---|---|
| 进料器 → 本机 | `<前缀><设备ID>/state` | 状态 JSON（含 `mac`、`ip`、`key`、`motor`…），retain |
| 进料器 → 本机 | `<前缀><设备ID>/availability` | online / offline，retain + 遗嘱 |
| 本机 → 进料器 | `<前缀>control` | `{"mac":"84:f3:eb:dd:27:00","start":"on"}`，payload 里带目标设备的 mac |

## 进料器那边要改的设置

每台的"局域网 MQTT"卡片里：

| 项 | 填什么 |
|---|---|
| 启用 | 勾上 |
| 允许远程控制 | **勾上**，否则它不订阅 `control`，网页上的按钮发过去也不会执行 |
| IP | 控制端页面上显示的"本机 IP"（例如 `192.168.5.218`） |
| 端口 | 和控制端一致，默认 `1883` |
| topic 前缀 | 和控制端一致，默认 `semiams/` |
| 用户名 / 密码 | 控制端填了账号就必须一致；控制端留空则进料器也留空 |

> 控制端走 DHCP，重启路由器后 IP 可能变。要么在路由器里给它绑一个固定 IP，
> 要么改完 IP 后把每台进料器的设置一起改。mDNS（`semi-ams-ctrl.local`）只有浏览器能用，
> 进料器固件不支持，它必须填 IP。

## 编译和烧录

```bash
cd controller
bash host_test/run.sh                   # 电脑上跑仿真测试（JSON 解析、mac、topic 通配符）
pio run                                 # 编译
pio run -t upload                       # 串口烧录（ESP32-C3 原生 USB，不用外接串口线）
pio device monitor                      # 串口日志
python3 tools/mqtt_selftest.py <IP> [端口] [用户名] [密码]      # 端到端自测（只用标准库）
python3 tools/mqtt_proto_check.py <IP> [端口] [用户名] [密码]  # 3.1 / 3.1.1 / 5.0 三种协议各跑一遍
python3 tools/mqtt_capacity.py <IP> [端口] [用户名] [密码] [试几个]  # 压同时在线数，看第几个被拒
python3 tools/mqtt_edge_check.py <IP> [端口] [用户名] [密码]    # QoS2 / 超长过滤器 / 假「已发送」/ 材料校验
```

- **协议版本**：服务端同时认 MQTT **3.1（MQIsdp/3）、3.1.1（4）、5.0（5）**。5.0 的差异都处理了：
  CONNECT/SUBSCRIBE/PUBLISH 里的属性块会跳过，CONNACK/SUBACK/UNSUBACK 会补上属性长度，
  下发给 5.0 客户端的 PUBLISH 也会带空属性块。

- 板子：`esp32-c3-devkitm-1`（4MB Flash），平台 `espressif32@6.9.0`、Arduino 框架

## 第一次配网

1. 上电，15 秒内连不上保存过的 WiFi（或从没配过）就开热点 **SEMI-AMS-CTRL**（开放）。
2. 手机或电脑连这个热点，打开 <http://192.168.4.1>，选/填 WiFi 名称和密码，保存。
3. 设备重启后连上路由器，用 <http://semi-ams-ctrl.local> 或路由器里查到的 IP 打开控制页面，
   串口日志里也会打印 `进料器的 MQTT 服务器就填 xxx.xxx.xxx.xxx`。

> ESP32-C3 只支持 2.4GHz。要重新配网：`pio run -t erase` 会清空 NVS（WiFi、服务端设置、设备表全没）。

## 控制页面

上面一张卡片是服务端设置，下面**按实际设备数**排的行：

| 项 | 说明 |
|---|---|
| 本机 IP | **只读**，就是进料器要填的地址；没联网时显示"未联网" |
| 端口 / 登录账号 / 登录密码 / Topic 前缀 | 改完点"保存并重连"，服务端按新端口重启监听；账号留空 = 允许匿名 |
| 设备 mac / 设备 ip | 从设备发的 `state` 里取，不是手工填的 |
| 设备状态 | `state` 的中文名；下面一行小字显示电机方向和占空比。离线时显示"离线 + 最后通信多久前" |
| 颜色 / 材料类型 | 自己标注用，存在控制端的 Flash 里（按 mac 绑定，换板不丢） |
| 电机控制 | 进料、退料、紧急停止、解除停止、删除、详情 |
| 添加设备 | 标题栏右侧，手动把一台填进表里：mac（冒号可有可无）+ 颜色 + 材料类型。适合先把设备排好、再逐台接进料器；加完等它上报一次消息就会变在线 |

按钮和协议里动作的对应关系：

| 按钮 | 发出去的 payload |
|---|---|
| 进料 | `{"mac":"…","start":"on"}` |
| 退料 | `{"mac":"…","start":"off"}` |
| 紧急停止 | `{"mac":"…","emg":"on"}` |
| 解除停止 | `{"mac":"…","emg":"off"}` |

设备离线时按钮会变灰。详情里还能看到开关、霍尔、信号、空闲内存、运行时长。

## 内置 MQTT 服务端

`src/broker.cpp`，纯手写的 MQTT 服务端（兼容 3.1 / 3.1.1 / 5.0）：

| 能力 | 说明 |
|---|---|
| CONNECT | 支持用户名密码校验（未设账号则允许匿名），拒绝时返回码 4 |
| SUBSCRIBE / UNSUBSCRIBE | 支持 `+` `#` 通配符，每个客户端最多 6 个过滤器；过滤器超过 47 字节直接回 `0x80`（失败），不截断 |
| PUBLISH | QoS 0 / 1 完整（收 QoS 1 回 PUBACK）；QoS 2 收下回 PUBREC 并投递一次，客户端发 PUBREL 后回 PUBCOMP，同 pid 的重发只补 PUBREC 不重复投递。转发统一按 QoS 0，不带 retain 位 |
| 保留消息 | 按 topic 存，最多 24 条 / 10 KB，超了淘汰最旧的；空 payload + retain 表示删除 |
| 遗嘱 LWT | 客户端异常断开（掉电、断网、keepalive 超时）代发；带 retain 的会存成保留消息 |
| PINGREQ | 回 PINGRESP，keepalive 按客户端声明的 1.5 倍判断超时 |
| 重复 clientId | 新连接踢掉旧的（MQTT 3.1.1 第 3.1.4 节） |

没做的：持久的 QoS 2 会话（断线不补发，只做单次握手）、持久会话（`cleanSession=0` 按 1 处理）、
共享订阅、TLS。

**容量**：代码里**没有客户端数上限**，来一台分配一个槽位（`broker.cpp` 里按需 `realloc` + `new`），
内存不够才拒。真正的天花板是 socket 数：ESP32-C3 的 `CONFIG_LWIP_MAX_SOCKETS=16`，这个常量在
IDF 静态库里编译好了，改头文件或加 `build_flags` 都不生效；16 个扣掉 2 个监听口（80 网页、1883 MQTT）
再给网页留 2 个并发请求，实际能同时在线 **12 个客户端**，第 13 个会在 TCP 握手时被直接关掉并打日志。

想再挤一台，把 `include/config.h` 里的 `MQTT_SOCKET_RESERVE_WEB` 改成 1（代价是客户端占满时网页
只剩一个 socket，可能打不开）；反过来，要多留余量就调大这个值。实测（`tools/mqtt_capacity.py`）：
12 个同时在线时再打开网页仍是 HTTP 200、耗时 0.2 秒，堆从 209 KB 降到 167 KB。

## 自测脚本

`tools/mqtt_selftest.py` 只用 Python 标准库实现了一个最小 MQTT 客户端，用来验证服务端：

```bash
python3 tools/mqtt_selftest.py 192.168.5.218 1883 用户名 密码
```

它依次检查：匿名被拒、密码错被拒、带账号能连、SUBACK、进料器上报 state（retain）能被订阅者收到、
网页上能看到这台设备、后连的客户端能收到保留消息、网页点"进料"能发出 `control`、
客户端掉线后遗嘱生效并让网页变离线、空 retain 能清掉保留消息。

## 代码结构

| 文件 | 内容 |
|---|---|
| `src/main.cpp` | 启动流程：读设置 → 连 WiFi（连不上开热点）→ 起网页 → 起 MQTT 服务端；运行中掉线每 10 秒重连，连续 3 分钟不回来就重启（开机连不上会自动开配网热点） |
| `src/broker.*` | MQTT 服务端：收包、鉴权、订阅、转发、保留消息、遗嘱 |
| `src/settings.*` | 服务端端口/账号/前缀、WiFi 账号，存 NVS |
| `src/devices.*` | 按需扩容的槽位、mac 绑定、在线超时、颜色和材料类型校验、逐行删除 |
| `src/webui.*` | REST 接口和页面路由 |
| `src/webpage.h` | 网页本身（PROGMEM，刷完就能用，不依赖 LittleFS） |
| `src/jsonmac.h` | 扁平 JSON 取值、mac 规范化（纯 C++，`host_test/` 里测的就是它） |
| `src/topic.h` | 主题过滤器匹配（`+` `#`），也是纯 C++ |
| `tools/mqtt_selftest.py` | 端到端自测脚本 |

REST 接口（页面自己用的，脚本也能调）：

| 接口 | 参数 | 作用 |
|---|---|---|
| `GET /api/state` | — | 本机 IP、WiFi、服务端状态、设备表全部字段 |
| `POST /api/config` | `port` `user` `pass` `prefix` | 保存并重启监听；返回 `running` / `status`，端口起不来时 `running:false` 并带原因 |
| `POST /api/label` | `mac`（或 `idx`）`color` `material` | 改颜色、材料类型；材料不能带 `"` `'` `<` `>` `&` `\`，颜色必须是 `#rrggbb` |
| `POST /api/control` | `mac`（或 `idx`）`cmd`（feed / retract / emg / emgclear / jogf / jogr / restart） | 给某台发命令。设备离线 → 409，没有人订阅 control 主题 → 503，成功时带 `sent`（实际送达台数） |
| `POST /api/adddev` | `mac` `color` `material` | 手动加一台，返回落位的 `idx`；mac 重复 / 32 台满了 / 颜色不是 `#rrggbb` / 材料带特殊字符都会返回 400 和中文原因 |
| `POST /api/deldev` | `mac`（或 `idx`） | 删掉一行，后面的行往前补一位，NVS 按新序号重写；只删记录，设备再上报消息会被重新加到最后一行 |

设备相关的三个接口都优先按 **mac** 定位（网页就是这么调的）：行号会随着删除往前挪，
两次轮询之间有人删了别的行的话，按行号提交会动到隔壁那台。`mac` 找不到时返回 409，
提示刷新页面；不带 `mac` 时退回按 `idx`，脚本和老页面照旧能用。

## 以后可以加的

- 自动选台：读打印机的外置料盘设置，决定这次让哪台送料（`firmware/docs/多台SEMI-AMS协同方案.md` 的做法一）。
- 批量操作（全部进料 / 全部退料）、打印进度、日志页。
- TLS / 更细的账号权限（现在只有一个账号，或者干脆匿名）。
