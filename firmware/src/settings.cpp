#include "settings.h"

#include <LittleFS.h>

#include "lanmqtt_proto.h"
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

// 解析 IPv4 并规范化写回 out；空字符串表示清空
static bool setIpv4(const char* v, char* out, size_t cap) {
  if (!v || !*v) {
    out[0] = 0;
    return true;
  }
  int a, b, c, d;
  char tail;
  if (sscanf(v, "%d.%d.%d.%d%c", &a, &b, &c, &d, &tail) != 4) return false;
  if (a < 0 || a > 255 || b < 0 || b > 255 || c < 0 || c > 255 || d < 0 || d > 255) return false;
  char buf[24];
  snprintf(buf, sizeof(buf), "%d.%d.%d.%d", a, b, c, d);
  strlcpy(out, buf, cap);
  return true;
}

bool settingsSetMqttIp(const char* v) { return setIpv4(v, gSettings.mqttIp, sizeof(gSettings.mqttIp)); }

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

bool settingsSetLanMqttIp(const char* v) { return setIpv4(v, gSettings.lanMqttIp, sizeof(gSettings.lanMqttIp)); }

bool settingsSetLanMqttPort(long p) {
  if (p < 1 || p > 65535) return false;
  gSettings.lanMqttPort = (uint16_t)p;
  return true;
}

// topic 前缀：规则见 semi::lan::normalizePrefix（可以在电脑上测试）
bool settingsSetLanMqttTopic(const char* v) {
  return semi::lan::normalizePrefix(v, gSettings.lanMqttTopic, sizeof(gSettings.lanMqttTopic));
}

// 用户名和密码只允许可打印 ASCII：要写进 settings.txt 的一行里，用户名还要回填到网页
static bool printableAscii(const char* v, size_t maxLen, const char* reject) {
  const size_t n = strlen(v);
  if (n > maxLen) return false;
  if (n && (v[0] == ' ' || v[n - 1] == ' ')) return false;  // 读文件时会去掉首尾空白
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)v[i];
    if (c < 0x20 || c > 0x7e || (reject && strchr(reject, c))) return false;
  }
  return true;
}

bool settingsSetLanMqttUser(const char* v) {
  if (!v) v = "";
  if (!printableAscii(v, sizeof(gSettings.lanMqttUser) - 1, "\"'<>&\\")) return false;
  strlcpy(gSettings.lanMqttUser, v, sizeof(gSettings.lanMqttUser));
  if (!*v) gSettings.lanMqttPass[0] = 0;  // 不认证时密码也没有意义
  return true;
}

bool settingsSetLanMqttPass(const char* v) {
  if (!v) v = "";
  if (!printableAscii(v, sizeof(gSettings.lanMqttPass) - 1, nullptr)) return false;
  strlcpy(gSettings.lanMqttPass, v, sizeof(gSettings.lanMqttPass));
  return true;
}

bool settingsLanMqttConfigured() { return gSettings.lanMqttEnable && gSettings.lanMqttIp[0]; }

// ---- 从旧分区迁移参数 ----
// v1.1.4 及以前按 4MB 分区编译（eagle.flash.4m1m.ld，文件系统在 0x300000、块大小 8KB）。这块模组的闪存
// 实际只有 2MB，超出的地址会绕回前 2MB，所以旧文件系统实际在 0x100000。v1.2.0 起改成 2MB 分区
// （eagle.flash.2m256.ld，文件系统在 0x1C0000），第一次启动时新文件系统是空的：从旧位置读出参数
// 写进去，然后擦掉旧文件系统的开头，保证只迁移一次。WiFi 和点灯库的 EEPROM 两种分区下位置相同，不用迁移
static const uint32_t kOldFsAddr = 0x100000, kOldFsSize = 0xFA000, kOldFsPage = 0x100, kOldFsBlock = 0x2000;
static const char* gBeginNote = nullptr;

static String readOldSettings() {
  String s;
  if (FS_PHYS_ADDR == kOldFsAddr || ESP.getFlashChipSize() < kOldFsAddr + kOldFsSize) return s;
  fs::FS oldFs(fs::FSImplPtr(new littlefs_impl::LittleFSImpl(kOldFsAddr, kOldFsSize, kOldFsPage, kOldFsBlock, 1)));
  oldFs.setConfig(LittleFSConfig(false));  // 旧位置不是文件系统时不要格式化它
  if (!oldFs.begin()) return s;
  File f = oldFs.open(kFile, "r");
  if (f) {
    s = f.readString();
    f.close();
  }
  oldFs.end();
  return s;
}

static void eraseOldFsHead() {
  // 旧文件系统的超级块在头两个 8KB 块里；这里现在是固件升级用的空闲区，擦掉不影响任何东西
  for (uint32_t a = kOldFsAddr; a < kOldFsAddr + 2 * kOldFsBlock; a += 0x1000) ESP.flashEraseSector(a / 0x1000);
}

const char* settingsBeginNote() { return gBeginNote; }

bool settingsBegin() {
  LittleFS.setConfig(LittleFSConfig(false));  // 挂载失败时先不自动格式化：要先试着迁移旧参数
  if (!LittleFS.begin()) {
    // 第一次使用、分区位置变了，或文件系统损坏
    const String old = readOldSettings();
    LittleFS.format();
    if (!LittleFS.begin()) return false;
    if (old.length()) {
      File w = LittleFS.open(kFile, "w");
      if (w) {
        w.print(old);
        w.close();
        eraseOldFsHead();
        gBeginNote = PSTR("文件系统换到新分区，参数已从旧分区迁移过来");
      }
    } else {
      gBeginNote = PSTR("文件系统是新建的，参数用默认值");
    }
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
    else if (k == "mqtt_auto_load") gSettings.mqttAutoLoad = v.toInt() != 0;
    else if (k == "mqtt_auto_unload") gSettings.mqttAutoUnload = v.toInt() != 0;
    else if (k == "lan_mqtt_enable") gSettings.lanMqttEnable = v.toInt() != 0;
    else if (k == "lan_mqtt_ip") settingsSetLanMqttIp(v.c_str());
    else if (k == "lan_mqtt_port") settingsSetLanMqttPort(v.toInt());
    else if (k == "lan_mqtt_topic") settingsSetLanMqttTopic(v.c_str());
    else if (k == "lan_mqtt_user") settingsSetLanMqttUser(v.c_str());
    else if (k == "lan_mqtt_pass") settingsSetLanMqttPass(v.c_str());
    else if (k == "lan_mqtt_control") gSettings.lanMqttControl = v.toInt() != 0;
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
  f.printf("mqtt_auto_load=%d\nmqtt_auto_unload=%d\n", gSettings.mqttAutoLoad ? 1 : 0, gSettings.mqttAutoUnload ? 1 : 0);
  f.printf("lan_mqtt_enable=%d\nlan_mqtt_ip=%s\nlan_mqtt_port=%u\nlan_mqtt_topic=%s\n", gSettings.lanMqttEnable ? 1 : 0,
           gSettings.lanMqttIp, (unsigned)gSettings.lanMqttPort, gSettings.lanMqttTopic);
  f.printf("lan_mqtt_user=%s\nlan_mqtt_pass=%s\nlan_mqtt_control=%d\n", gSettings.lanMqttUser, gSettings.lanMqttPass,
           gSettings.lanMqttControl ? 1 : 0);
  f.close();
  return true;
}
