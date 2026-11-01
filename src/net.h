// WiFi 配网（SEMI-AMS 热点）、设备网页、OTA 升级
#pragma once
#include <Arduino.h>

namespace net {
void begin();  // 非阻塞：连不上就开配网热点，状态机照常运行
void loop();
bool connected();
String ip();
void resetAll();  // 清除 WiFi 和点灯密钥后重启
}  // namespace net
