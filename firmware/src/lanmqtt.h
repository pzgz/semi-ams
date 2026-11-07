// 局域网 MQTT：把本机状态发布给局域网里的 MQTT 服务器（Home Assistant / Node-RED 等）。
// 和连接拓竹打印机的 MQTT 是两回事：这里是普通 TCP（默认 1883），不加密、不需要认证。
// 状态用 QoS 0 发布；连上服务器后订阅 <前缀>control 接收控制命令（QoS 1）。
//
// topic：
//   <前缀>state         状态 JSON，retain
//   <前缀>availability  online / offline，retain（offline 走遗嘱，掉电时由服务器代发）
//   <前缀>control       控制命令，收到后先核对 mac 是否为本机，再执行 start / emg / jogf / jogr / restart
#pragma once
#include <Arduino.h>

namespace lanmqtt {
void loop();            // loop() 里调用；启用且 WiFi 连上后才工作
void applySettings();   // 设置改了：断开重连
bool enabled();         // 启用且填了服务器 IP
bool connected();
String statusText();    // 未启用 / 未填 IP / 连接中 / 已连接 ip:端口 / x 秒后重连（原因）
uint32_t publishCount();
const char* lastError();  // 最近一次失败原因（内存里的字符串），没有时为空
}  // namespace lanmqtt
