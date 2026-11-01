// 可调参数：保存在 LittleFS 的 /settings.txt（key=value），网页和串口都能修改
#pragma once
#include <Arduino.h>

#include "fsm.h"

struct Settings {
  semi::Params p;
  bool motorInvert = false;
  char blinkerKey[33] = "";
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
