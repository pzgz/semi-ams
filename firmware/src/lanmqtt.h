// 局域网 MQTT：把本机状态发布给局域网里的 MQTT 服务器（Home Assistant / Node-RED / 群控程序等）。
// 和连接拓竹打印机的 MQTT 是两回事：这里是普通 TCP（默认 1883），不加密，可选用户名和密码。
//
// topic（详见 lanmqtt_proto.h 和 docs/局域网MQTT消息参考.md）：
//   <前缀><设备ID>/state         状态 JSON，retain
//   <前缀><设备ID>/availability  {"state":"online|offline","ip":…,"mac":…}，retain（offline 走遗嘱）
//   <前缀>control                控制命令（开了"允许远程控制"才订阅），mac 等于本机才执行
#pragma once
#include <Arduino.h>

namespace lanmqtt {
void loop();            // loop() 里调用；启用且 WiFi 连上后才工作
void applySettings();   // 设置改了：断开重连
bool enabled();         // 启用且填了服务器 IP
bool connected();
String statusText();    // 未启用 / 未填服务器 IP / 等点灯初始化完成 / 连接中 / 已连接 ip:端口 / x 秒后重连（原因）
String topicText();     // 本机的状态 topic，例如 semiams/84f3ebdd2700/state（网页上显示）
uint32_t publishCount();
const char* lastError();  // 最近一次失败原因（内存里的字符串），没有时为空
}  // namespace lanmqtt
