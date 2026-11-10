#include "devices.h"

#include <Preferences.h>
#include <ctype.h>
#include <stdlib.h>

#include "jsonmac.h"

namespace ctrl {

namespace {

// 表是动态的：来一台追加一个槽位，行数跟着设备数走（不再固定 8 行）。
// 删掉一行时后面的槽位整体前移，NVS 里的 mac0/col0/mat0… 按新序号重写。
Device* gDev = nullptr;
int devCount = 0;  // 已用槽位数 = 界面行数
int devCap = 0;    // 已分配的槽位容量

// 默认颜色：只有 8 个色，超过 8 台就循环用（原来的写法会越界取到空指针）
const char* kPalette[] = {"#3b6fd4", "#c0392b", "#2f9e44", "#e0b400",
                          "#7d4bb3", "#3aa99a", "#e07b39", "#2c3e50"};
const int kPaletteCount = (int)(sizeof(kPalette) / sizeof(kPalette[0]));

void clearDevice(Device& d, int idx) {
  memset(&d, 0, sizeof(d));
  strncpy(d.color, kPalette[idx % kPaletteCount], sizeof(d.color) - 1);
  d.material[0] = 0;
  d.rssi = 0;
  d.lastSeenMs = 0;
}

// 追加一个槽位；到 DEVICES_MAX（存储保护上限）或内存不够时返回 nullptr
Device* appendSlot() {
  if (devCount >= DEVICES_MAX) return nullptr;
  if (devCount >= devCap) {
    int cap = devCap ? devCap * 2 : 4;
    if (cap > DEVICES_MAX) cap = DEVICES_MAX;
    Device* p = (Device*)realloc(gDev, (size_t)cap * sizeof(Device));
    if (!p) {
      Serial.printf("[设备表] 扩容失败（要 %d 个槽位，堆剩 %u）\n", cap,
                    (unsigned)ESP.getFreeHeap());
      return nullptr;
    }
    gDev = p;
    devCap = cap;
  }
  clearDevice(gDev[devCount], devCount);
  return &gDev[devCount++];
}

void saveMac(int idx) {
  Preferences p;
  p.begin("dev", false);
  char k[8];
  snprintf(k, sizeof(k), "mac%d", idx);
  p.putString(k, gDev[idx].mac);
  p.end();
}

void saveLabel(int idx) {
  Preferences p;
  p.begin("dev", false);
  char k[8];
  snprintf(k, sizeof(k), "col%d", idx);
  p.putString(k, gDev[idx].color);
  snprintf(k, sizeof(k), "mat%d", idx);
  p.putString(k, gDev[idx].material);
  p.end();
}

void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

// 颜色必须是 #rrggbb 且六位都是十六进制，否则拼到网页样式里会出怪东西
bool validColor(const String& c) {
  if (c.length() != 7 || c[0] != '#') return false;
  for (int i = 1; i < 7; ++i)
    if (!isxdigit((unsigned char)c[i])) return false;
  return true;
}

// 材料类型：15 字节以内，不能有引号 / 尖括号 / & / 反斜杠 / 控制字符
// （这些字符会破坏 state 的 JSON，也可能被网页当成标签；settings 里是同一套规则）
bool validMaterial(const String& s, String& err) {
  if (s.length() > 15) {
    err = "材料类型最长 15 字节（约 5 个汉字）";
    return false;
  }
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if ((unsigned char)c < 0x20) {
      err = "材料类型不能有控制字符";
      return false;
    }
    if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' || c == '\\') {
      err = "材料类型不能有 \" ' < > & \\";
      return false;
    }
  }
  return true;
}

// 放一个设备进表：先找已有的 mac，没有再追加一行
Device* slotFor(const char* mac18) {
  for (int i = 0; i < devCount; ++i) {
    if (gDev[i].mac[0] && strcmp(gDev[i].mac, mac18) == 0) return &gDev[i];
  }
  Device* d = appendSlot();
  if (!d) {
    Serial.printf("[设备表] 加不下 %s（已 %d 台，上限 %d）\n", mac18, devCount, DEVICES_MAX);
    return nullptr;
  }
  copyStr(d->mac, sizeof(d->mac), mac18);
  saveMac(devCount - 1);
  Serial.printf("[设备表] 新设备 %s 落在第 %d 行\n", mac18, devCount - 1 + 1);
  return d;
}

void applyCommon(Device& d, const char* js) {
  char buf[40];
  if (semi::json::get(js, "ip", buf, sizeof(buf))) copyStr(d.ip, sizeof(d.ip), buf);
  long v;
  if (semi::json::getLong(js, "rssi", &v)) d.rssi = v;
  if (semi::json::getLong(js, "heap", &v)) d.heap = v;
  if (semi::json::getLong(js, "uptime", &v)) d.uptime = v;
}

}  // namespace

void devicesBegin() {
  Preferences p;
  p.begin("dev", true);
  bool moved = false;  // NVS 里老序号有空洞，压缩后要重写
  // 从头读到尾，只把 mac 非空的压紧成连续槽位（兼容老固件 8 行里中间留空的情况）
  for (int i = 0; i < DEVICES_MAX; ++i) {
    char k[8];
    snprintf(k, sizeof(k), "mac%d", i);
    String mac = p.getString(k, "");
    if (!mac.length()) continue;
    snprintf(k, sizeof(k), "col%d", i);
    String col = p.getString(k, "");
    snprintf(k, sizeof(k), "mat%d", i);
    String mat = p.getString(k, "");

    Device* d = appendSlot();
    if (!d) break;
    copyStr(d->mac, sizeof(d->mac), mac.c_str());
    if (col.length()) copyStr(d->color, sizeof(d->color), col.c_str());
    if (mat.length()) copyStr(d->material, sizeof(d->material), mat.c_str());
    if (devCount - 1 != i) moved = true;
  }
  p.end();

  if (moved) {  // 按新序号补写一遍（写之前先关掉读句柄，避免同一 namespace 两个句柄）
    for (int i = 0; i < devCount; ++i) {
      saveMac(i);
      saveLabel(i);
    }
  }
  Serial.printf("[设备表] 载入 %d 台（存储上限 %d）\n", devCount, DEVICES_MAX);
}

Device& deviceAt(int i) { return gDev[i]; }

int devicesUsed() { return devCount; }

int devicesOnline() {
  int n = 0;
  for (int i = 0; i < devCount; ++i)
    if (gDev[i].mac[0] && gDev[i].online) ++n;
  return n;
}

void devicesOnState(const char* payload) {
  if (!payload) return;
  char rawMac[24];
  char mac18[18];
  if (!semi::json::get(payload, "mac", rawMac, sizeof(rawMac))) {
    Serial.println("[设备表] state 里没有 mac，忽略");
    return;
  }
  if (!semi::mac::normalize(rawMac, mac18, sizeof(mac18))) {
    Serial.printf("[设备表] mac 认不出来：%s\n", rawMac);
    return;
  }
  Device* d = slotFor(mac18);
  if (!d) {
    Serial.printf("[设备表] 加不下 %s，忽略这条 state\n", mac18);
    return;
  }

  char buf[40];
  if (semi::json::get(payload, "state", buf, sizeof(buf))) copyStr(d->state, sizeof(d->state), buf);
  if (semi::json::get(payload, "key", buf, sizeof(buf))) copyStr(d->key, sizeof(d->key), buf);
  if (semi::json::get(payload, "motor", buf, sizeof(buf))) copyStr(d->motor, sizeof(d->motor), buf);
  long v;
  if (semi::json::getLong(payload, "duty", &v)) d->duty = (int)v;
  if (semi::json::getLong(payload, "in", &v)) d->in = (int)v;
  if (semi::json::getLong(payload, "ready", &v)) d->ready = (int)v;
  if (semi::json::getLong(payload, "lack", &v)) d->lack = (int)v;
  if (semi::json::getLong(payload, "full", &v)) d->full = (int)v;
  applyCommon(*d, payload);
  d->online = true;
  d->lastSeenMs = millis();
}

void devicesOnAvailability(const char* payload) {
  if (!payload) return;
  char rawMac[24];
  char mac18[18];
  if (!semi::json::get(payload, "mac", rawMac, sizeof(rawMac))) return;
  if (!semi::mac::normalize(rawMac, mac18, sizeof(mac18))) return;
  Device* d = slotFor(mac18);
  if (!d) return;

  char st[16];
  if (!semi::json::get(payload, "state", st, sizeof(st))) return;
  applyCommon(*d, payload);
  if (strcmp(st, "offline") == 0) {
    if (d->online) Serial.printf("[设备表] %s 掉线（服务器的遗嘱）\n", d->mac);
    d->online = false;
  } else {
    d->online = true;
    d->lastSeenMs = millis();
  }
}

void devicesLoop() {
  const uint32_t now = millis();
  for (int i = 0; i < devCount; ++i) {
    Device& d = gDev[i];
    if (!d.mac[0] || !d.online) continue;
    if (now - d.lastSeenMs > DEVICE_OFFLINE_MS) {
      d.online = false;
      Serial.printf("[设备表] %s %u 秒没有消息，当作离线\n", d.mac,
                    (unsigned)(DEVICE_OFFLINE_MS / 1000));
    }
  }
}

bool devicesSetLabel(int idx, const char* color, const char* material, String& err) {
  if (idx < 0 || idx >= devCount) {
    err = "槽位序号不对";
    return false;
  }
  Device& d = gDev[idx];
  if (!d.mac[0]) {
    err = "这一行还没有设备";
    return false;
  }
  if (color && color[0]) {
    if (!validColor(color)) {
      err = "颜色要写成 #rrggbb";
      return false;
    }
    copyStr(d.color, sizeof(d.color), color);
  }
  if (material) {
    String s = material;
    s.trim();
    if (!validMaterial(s, err)) return false;
    copyStr(d.material, sizeof(d.material), s.c_str());
  }
  saveLabel(idx);
  return true;
}

bool devicesAdd(const char* mac, const char* color, const char* material, int& idxOut, String& err) {
  idxOut = -1;
  char mac18[18];
  if (!semi::mac::normalize(mac, mac18, sizeof(mac18))) {
    err = "mac 要 12 位十六进制，冒号可有可无";
    return false;
  }
  for (int i = 0; i < devCount; ++i) {
    if (gDev[i].mac[0] && strcmp(gDev[i].mac, mac18) == 0) {
      err = "这个 mac 已经在第 " + String(i + 1) + " 行";
      return false;
    }
  }
  if (color && color[0] && !validColor(color)) {
    err = "颜色要写成 #rrggbb";
    return false;
  }
  String mat = material ? material : "";
  mat.trim();
  if (!validMaterial(mat, err)) return false;

  Device* pd = appendSlot();
  if (!pd) {
    err = "设备表已经 " + String(DEVICES_MAX) + " 台了，先删掉几台再加";
    return false;
  }
  const int slot = devCount - 1;
  Device& d = *pd;
  copyStr(d.mac, sizeof(d.mac), mac18);
  if (color && color[0]) copyStr(d.color, sizeof(d.color), color);
  copyStr(d.material, sizeof(d.material), mat.c_str());
  saveMac(slot);
  saveLabel(slot);
  idxOut = slot;
  Serial.printf("[设备表] 手动添加 %s 到第 %d 行（%s / %s）\n", mac18, slot + 1, d.color, d.material);
  return true;
}

bool devicesRemove(int idx, String& err) {
  if (idx < 0 || idx >= devCount) {
    err = "槽位序号不对";
    return false;
  }
  if (!gDev[idx].mac[0]) {
    err = "这一行还没有设备";
    return false;
  }
  char mac[18];
  copyStr(mac, sizeof(mac), gDev[idx].mac);

  // 后面的槽位整体前移一位，行号保持连续（NVS 里的序号也要跟着挪）
  for (int i = idx; i < devCount - 1; ++i) gDev[i] = gDev[i + 1];
  --devCount;
  memset(&gDev[devCount], 0, sizeof(Device));  // 让出末位，下次添加时干净复用

  Preferences p;
  p.begin("dev", false);
  char k[8];
  for (int i = 0; i < devCount; ++i) {
    snprintf(k, sizeof(k), "mac%d", i);
    p.putString(k, gDev[i].mac);
    snprintf(k, sizeof(k), "col%d", i);
    p.putString(k, gDev[i].color);
    snprintf(k, sizeof(k), "mat%d", i);
    p.putString(k, gDev[i].material);
  }
  // 刚空出来的末位记录要删掉，不然重启后又被载入
  snprintf(k, sizeof(k), "mac%d", devCount);
  p.remove(k);
  snprintf(k, sizeof(k), "col%d", devCount);
  p.remove(k);
  snprintf(k, sizeof(k), "mat%d", devCount);
  p.remove(k);
  p.end();

  Serial.printf("[设备表] 删除 %s（原第 %d 行），剩 %d 台\n", mac, idx + 1, devCount);
  return true;
}

int devicesFindByMacId(const char* macId) {
  char mac18[18];
  if (!semi::mac::normalize(macId, mac18, sizeof(mac18))) return -1;
  for (int i = 0; i < devCount; ++i) {
    if (gDev[i].mac[0] && strcmp(gDev[i].mac, mac18) == 0) return i;
  }
  return -1;
}

}  // namespace ctrl
