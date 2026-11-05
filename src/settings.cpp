#include "settings.h"

#include <LittleFS.h>
#include <pgmspace.h>
#include <ctype.h>
#include <stdio.h>

Settings gSettings;

static const char* kFile = "/settings.txt";

// 参数的中文名放在 Flash 里（PROGMEM），读取时用 FPSTR() 或 %S
static const char kL_load_speed[] PROGMEM = "吸入速度";
static const char kL_feed_speed[] PROGMEM = "补料速度";
static const char kL_unload_speed[] PROGMEM = "退料速度";
static const char kL_motor_invert[] PROGMEM = "电机方向反转";
static const char kL_insert_delay_ms[] PROGMEM = "插入后延时吸入";
static const char kL_load_timeout_s[] PROGMEM = "吸入超时";
static const char kL_first_feed_timeout_s[] PROGMEM = "开始打印首次送料超时";
static const char kL_feed_timeout_s[] PROGMEM = "打印中补料超时";
static const char kL_unload_timeout_s[] PROGMEM = "退料超时";
static const char kL_unload_extra_ms[] PROGMEM = "退料离开就绪开关后多退";
static const char kL_tail_feed_ms[] PROGMEM = "耗材用尽后送出料尾最长";

static const SettingDef kDefs[] = {
    {"load_speed", kL_load_speed, "%", 10, 100},
    {"feed_speed", kL_feed_speed, "%", 10, 100},
    {"unload_speed", kL_unload_speed, "%", 10, 100},
    {"motor_invert", kL_motor_invert, "0/1", 0, 1},
    {"insert_delay_ms", kL_insert_delay_ms, "ms", 0, 5000},
    {"load_timeout_s", kL_load_timeout_s, "s", 3, 600},
    {"first_feed_timeout_s", kL_first_feed_timeout_s, "s", 5, 900},
    {"feed_timeout_s", kL_feed_timeout_s, "s", 3, 600},
    {"unload_timeout_s", kL_unload_timeout_s, "s", 5, 900},
    {"unload_extra_ms", kL_unload_extra_ms, "ms", 0, 10000},
    {"tail_feed_ms", kL_tail_feed_ms, "ms", 0, 30000},
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
  if (!strcmp(key, "tail_feed_ms")) return p.tailFeedMs;
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
  else if (!strcmp(key, "tail_feed_ms")) p.tailFeedMs = v;
  else return false;
  return true;
}

void settingsSetBlinkerKey(const char* key) {
  strlcpy(gSettings.blinkerKey, key ? key : "", sizeof(gSettings.blinkerKey));
}

static bool alnum(const char* v, size_t minLen, size_t maxLen) {
  const size_t n = v ? strlen(v) : 0;
  if (n < minLen || n > maxLen) return false;
  for (size_t i = 0; i < n; i++)
    if (!isalnum((unsigned char)v[i])) return false;
  return true;
}

bool settingsSetMqttIp(const char* v) {
  if (!v || !*v) {
    gSettings.mqttIp[0] = 0;
    return true;
  }
  int a, b, c, d;
  char tail;
  if (sscanf(v, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  char buf[24];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", a, b, c, d);
  strlcpy(gSettings.mqttIp, buf, sizeof(gSettings.mqttIp));
  return true;
}

bool settingsSetMqttSn(const char* v) {
  if (v && *v && !alnum(v, 8, 20)) return false;
  strlcpy(gSettings.mqttSn, v ? v : "", sizeof(gSettings.mqttSn));
  for (char* p = gSettings.mqttSn; *p; p++) *p = toupper((unsigned char)*p);  // 序列号是大写
  return true;
}

bool settingsSetMqttCode(const char* v) {
  if (v && *v && !alnum(v, 6, 16)) return false;
  strlcpy(gSettings.mqttCode, v ? v : "", sizeof(gSettings.mqttCode));
  return true;
}

bool settingsSetMqttUnloadWait(long s) {
  if (s < 0 || s > 600) return false;
  gSettings.mqttUnloadWaitS = (uint16_t)s;
  return true;
}

bool settingsMqttConfigured() {
  return gSettings.mqttIp[0] && gSettings.mqttSn[0] && gSettings.mqttCode[0];
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
    else if (k == "mqtt_enable") gSettings.mqttEnable = v.toInt() != 0;
    else if (k == "mqtt_ip") settingsSetMqttIp(v.c_str());
    else if (k == "mqtt_sn") settingsSetMqttSn(v.c_str());
    else if (k == "mqtt_code") settingsSetMqttCode(v.c_str());
    else if (k == "mqtt_unload_wait_s") settingsSetMqttUnloadWait(v.toInt());
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
  f.printf("mqtt_enable=%d\nmqtt_ip=%s\nmqtt_sn=%s\nmqtt_code=%s\nmqtt_unload_wait_s=%u\n",
           gSettings.mqttEnable ? 1 : 0, gSettings.mqttIp, gSettings.mqttSn, gSettings.mqttCode,
           (unsigned)gSettings.mqttUnloadWaitS);
  f.close();
  return true;
}
