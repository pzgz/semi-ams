#include "settings.h"

#include <LittleFS.h>

Settings gSettings;

static const char* kFile = "/settings.txt";

static const SettingDef kDefs[] = {
    {"load_speed", "吸入速度", "%", 10, 100},
    {"feed_speed", "补料速度", "%", 10, 100},
    {"unload_speed", "退料速度", "%", 10, 100},
    {"motor_invert", "电机方向反转", "0/1", 0, 1},
    {"insert_delay_ms", "插入后延时吸入", "ms", 0, 5000},
    {"load_timeout_s", "吸入超时", "s", 3, 600},
    {"first_feed_timeout_s", "开始打印首次送料超时", "s", 5, 900},
    {"feed_timeout_s", "打印中补料超时", "s", 3, 600},
    {"unload_timeout_s", "退料超时", "s", 5, 900},
    {"unload_extra_ms", "退料离开就绪开关后多退", "ms", 0, 10000},
};

const SettingDef* settingDefs(size_t& count) {
  count = sizeof(kDefs) / sizeof(kDefs[0]);
  return kDefs;
}

static const SettingDef* findDef(const char* key) {
  for (const auto& d : kDefs)
    if (strcmp(d.key, key) == 0) return &d;
  return nullptr;
}

long settingsGet(const char* key) {
  const semi::Params& p = gSettings.p;
  if (!strcmp(key, "load_speed")) return p.loadSpeed;
  if (!strcmp(key, "feed_speed")) return p.feedSpeed;
  if (!strcmp(key, "unload_speed")) return p.unloadSpeed;
  if (!strcmp(key, "motor_invert")) return gSettings.motorInvert ? 1 : 0;
  if (!strcmp(key, "insert_delay_ms")) return p.insertDelayMs;
  if (!strcmp(key, "load_timeout_s")) return p.loadTimeoutMs / 1000;
  if (!strcmp(key, "first_feed_timeout_s")) return p.firstFeedTimeoutMs / 1000;
  if (!strcmp(key, "feed_timeout_s")) return p.feedTimeoutMs / 1000;
  if (!strcmp(key, "unload_timeout_s")) return p.unloadTimeoutMs / 1000;
  if (!strcmp(key, "unload_extra_ms")) return p.unloadExtraMs;
  return -1;
}

bool settingsSet(const char* key, long v) {
  const SettingDef* d = findDef(key);
  if (!d || v < d->min || v > d->max) return false;
  semi::Params& p = gSettings.p;
  if (!strcmp(key, "load_speed")) p.loadSpeed = v;
  else if (!strcmp(key, "feed_speed")) p.feedSpeed = v;
  else if (!strcmp(key, "unload_speed")) p.unloadSpeed = v;
  else if (!strcmp(key, "motor_invert")) gSettings.motorInvert = v != 0;
  else if (!strcmp(key, "insert_delay_ms")) p.insertDelayMs = v;
  else if (!strcmp(key, "load_timeout_s")) p.loadTimeoutMs = v * 1000UL;
  else if (!strcmp(key, "first_feed_timeout_s")) p.firstFeedTimeoutMs = v * 1000UL;
  else if (!strcmp(key, "feed_timeout_s")) p.feedTimeoutMs = v * 1000UL;
  else if (!strcmp(key, "unload_timeout_s")) p.unloadTimeoutMs = v * 1000UL;
  else if (!strcmp(key, "unload_extra_ms")) p.unloadExtraMs = v;
  else return false;
  return true;
}

void settingsSetBlinkerKey(const char* key) {
  strlcpy(gSettings.blinkerKey, key ? key : "", sizeof(gSettings.blinkerKey));
}

bool settingsBegin() {
  if (!LittleFS.begin()) {
    // 第一次使用或分区损坏：格式化后再挂载
    LittleFS.format();
    if (!LittleFS.begin()) return false;
  }
  File f = LittleFS.open(kFile, "r");
  if (!f) return true;  // 没有文件就用默认值
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    int eq = line.indexOf('=');
    if (eq <= 0) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    k.trim();
    v.trim();
    if (k == "blinker_key") settingsSetBlinkerKey(v.c_str());
    else settingsSet(k.c_str(), v.toInt());
  }
  f.close();
  return true;
}

bool settingsSave() {
  File f = LittleFS.open(kFile, "w");
  if (!f) return false;
  for (const auto& d : kDefs) {
    f.print(d.key);
    f.print('=');
    f.println(settingsGet(d.key));
  }
  f.print("blinker_key=");
  f.println(gSettings.blinkerKey);
  f.close();
  return true;
}
