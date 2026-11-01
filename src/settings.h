// 可调参数：保存在 LittleFS 的 /settings.txt（key=value），网页和串口都能修改
#pragma once
#include <Arduino.h>

#include "fsm.h"

struct Settings {
  semi::Params p;
  bool motorInvert = false;
  char blinkerKey[33] = "";
  // 拓竹打印机（MQTT）
  bool mqttEnable = false;
  char mqttIp[16] = "";     // 打印机 IP
  char mqttSn[24] = "";     // 打印机序列号
  char mqttCode[17] = "";   // 打印机访问码（配对码）
  uint16_t mqttUnloadWaitS = 120;  // 打印机退料后，最多等多久再开始退料（等它把耗材退出自己的传感器）
};

struct SettingDef {
  const char* key;
  const char* label;
  const char* unit;
  long min;
  long max;
};

extern Settings gSettings;

// 网页 / 串口可调的数值参数表
const SettingDef* settingDefs(size_t& count);

bool settingsBegin();  // 挂载文件系统并读取
bool settingsSave();
long settingsGet(const char* key);
// 设置一个数值参数；超出范围或未知 key 返回 false
bool settingsSet(const char* key, long value);
void settingsSetBlinkerKey(const char* key);

// 拓竹打印机设置：格式不对返回 false 且不修改
bool settingsSetMqttIp(const char* v);    // IPv4，例如 192.168.1.50
bool settingsSetMqttSn(const char* v);    // 字母和数字，8–20 位
bool settingsSetMqttCode(const char* v);  // 字母和数字，6–16 位（拓竹一般是 8 位）
bool settingsSetMqttUnloadWait(long s);   // 0–600 秒
bool settingsMqttConfigured();            // IP、序列号、访问码都已填写
