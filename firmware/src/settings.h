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
  // 多台 SEMI-AMS 接同一台打印机时用：不在这次打印里用的那几台关掉"响应进料"，否则开始打印时会一起送料。
  // 退料只有正在"打印中"的那台会执行，一般都开着
  bool mqttAutoLoad = true;    // 响应进料：打印机用外置料盘开始打印时自动送料
  bool mqttAutoUnload = true;  // 响应退料：在打印机上退料时自动退料
  // 局域网 MQTT：把本机状态发布给局域网里的 MQTT 服务器（Home Assistant / Node-RED 等），
  // 和上面连接打印机用的 MQTT 是两回事，互不影响
  bool lanMqttEnable = false;
  char lanMqttIp[16] = "";              // 服务器 IP
  uint16_t lanMqttPort = 1883;          // 服务器端口
  char lanMqttTopic[33] = "semiams/";   // topic 前缀，结尾一定带 /
};

struct SettingDef {
  const char* key;
  const char* label;  // 中文名，在 Flash 里（PROGMEM）：用 FPSTR() 或 %S 读
  const char* unit;
  long min;
  long max;
};

extern Settings gSettings;

// 网页 / 串口可调的数值参数表
const SettingDef* settingDefs(size_t& count);

bool settingsBegin();  // 挂载文件系统并读取
const char* settingsBeginNote();  // 新建了文件系统或迁移了参数时的说明（在 Flash 里，用 %S 读），否则 nullptr
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

// 局域网 MQTT 设置：格式不对返回 false 且不修改
bool settingsSetLanMqttIp(const char* v);     // IPv4，例如 192.168.5.10
bool settingsSetLanMqttPort(long p);          // 1–65535
bool settingsSetLanMqttTopic(const char* v);  // topic 前缀，留空恢复默认；结尾自动补 /
bool settingsLanMqttConfigured();             // 已启用且填了 IP
