# 局域网 MQTT 消息参考

## 消息发送

#### 设备状态信息Topic
前缀 + ``state``；状态变化时发（最小间隔 500 ms），没变化每 30 s 补一份心跳

```json
{
  "state": "无耗材",
  "motor": "停止",
  "duty": 0,
  "in": 0,
  "ready": 0,
  "lack": 0,
  "full": 0,
  "rssi": -77,
  "heap": 24792,
  "uptime": 126,
  "ip": "192.168.5.222",
  "mac": "84:f3:eb:dd:27:00"
}
```

> 逻辑
```text
每轮 loop
已连接时
 ↓
≥ 500 ms
距上次成功发布
 ↓
rev 变了 或 ≥ 30 s
否则直接跳过本轮
 ↓
发布
retain
```

#### mqtt服务器掉线检测Topic
``前缀 + availability``

> 逻辑
```text
连上的那一刻（CONNACK 返回 0）
availability = online（retain），只发这一次
此后直到断开，不会再碰这个 topic
 ↓
在线期间 —— 有周期的只有这里
state：状态变了立刻发，最小间隔 500 ms
state：没变化也每 30 s 补一份，当心跳用
PINGREQ：每 30 s 保活，90 s 收不到数据判死
 ↓
断开的时候
主动断开（改设置、超时）→ 本机发 offline（retain）
掉电 / 断网 / 崩溃 → 服务器按遗嘱代发 offline
```

## 消息订阅

> Topic

前缀 + ``control``

执行操作前需要先判断数据里的mac是否等于本设备mac

#### 开始/结束打印
```text
{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "start": "on"
}

{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "start": "off"
}
```
#### 紧急停止/解除
```text
{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "emg": "on"
}

{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "emg": "off"
}
```

#### 正转2秒
```json
{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "jogf": "on"
}
```

#### 反转2秒
```json
{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "jogr": "on"
}
```

#### 重启
```json
{
  "ip": "192.168.5.222",
  "mac": "84:f4:eb:dd:27:00",
  "restart": "on"
}
```