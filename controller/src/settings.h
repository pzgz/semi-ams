// 控制端的设置：内置 MQTT 服务端的端口 / 账号、WiFi 账号，都存在 NVS（Flash）里
#pragma once
#include <Arduino.h>

namespace ctrl {

struct Settings {
  uint16_t brokerPort;  // 本机 MQTT 服务端端口，默认 1883
  char user[33];        // 空 = 允许匿名连接
  char pass[65];
  char prefix[33];      // topic 前缀，结尾保证有 '/'，默认 "semiams/"
  bool autoRetract;     // 监听区勾选项：打印机报退料卡住时自动让「打印中」的进料器反转
};

extern Settings gSettings;

void settingsBegin();
void settingsSave();

// 校验并写入；出错返回 false，err 里写原因（给网页显示）
bool settingsSetServer(const char* port, const char* user, const char* pass, const char* prefix,
                       String& err);

// 自动退料开关单独存：它和上面那组是两次独立的操作，不该互相覆盖
void settingsSetAutoRetract(bool on);

// WiFi 账号（配网热点里填的）
bool wifiLoad(String& ssid, String& pass);
void wifiSave(const String& ssid, const String& pass);
void wifiClear();

}  // namespace ctrl
