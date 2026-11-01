// 拓竹打印机（MQTT）：连接打印机本机的 MQTT（端口 8883，TLS，用户名 bblp，密码为访问码），
// 订阅 device/<序列号>/report。打印机用外置料盘开始打印时自动送料，打印机退料时自动退料。
// 只读取打印机的消息（连上后发一次 pushall 请求完整状态），不控制打印机，
// 打印机可以保持连接拓竹云，不需要"仅局域网"或开发者模式。
#pragma once
#include <Arduino.h>

namespace printer {
void begin();
void loop();             // 在 loop() 里调用；WiFi 连上后才工作
void applySettings();    // 打印机设置改变后调用：断开并重新连接
void setEnabled(bool on);  // 开关 MQTT 功能（保存设置）
bool enabled();
bool connected();
uint32_t revision();     // 连接状态变化时改变（点灯刷新按钮用）
String statusText();     // 未启用 / 未设置 / 连接中 / 已连接 / x 秒后重连（原因）
String printerText();    // 打印机状态摘要
const char* lastEvent(); // 最近一次自动送料 / 退料
}  // namespace printer
