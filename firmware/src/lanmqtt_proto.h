// 局域网 MQTT 里不依赖硬件的部分（topic 前缀、设备 ID、控制命令解析），纯 C++，可以在电脑上测试。
//
// topic（<前缀> 默认 semiams/，<设备ID> 是 MAC 去掉冒号的小写，例如 84f3ebdd2700）：
//   <前缀><设备ID>/state         状态 JSON，retain
//   <前缀><设备ID>/availability  {"state":"online|offline","ip":…,"mac":…}，retain；offline 走遗嘱
//   <前缀>control                控制命令，几台共用，payload 里的 mac 等于本机才执行
// 每台的状态和在线情况要用各自的 topic：服务器的保留消息按 topic 只存一条，几台共用一个 topic
// 会互相覆盖。群控订阅 <前缀>+/state、<前缀>+/availability 就能收到所有设备，身份看 payload 里的 mac。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "json_scan.h"

namespace semi {
namespace lan {

static const char kDefaultPrefix[] = "semiams/";

// 规范化 topic 前缀：留空恢复默认，结尾补 /。拒绝 MQTT 通配符（+ #）、空白、控制字符，
// 以及会破坏网页和 JSON 的 " ' < > & \。中文等 UTF-8 允许。格式不对返回 false，不修改 out
inline bool normalizePrefix(const char* v, char* out, size_t cap) {
  if (!v || !out || cap < sizeof(kDefaultPrefix)) return false;
  if (!*v) {
    strcpy(out, kDefaultPrefix);
    return true;
  }
  const size_t n = strlen(v);
  const bool slash = v[n - 1] == '/';
  if (n + (slash ? 0 : 1) + 1 > cap) return false;
  for (size_t i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)v[i];
    if (c <= 0x20 || c == 0x7f || strchr("+#\"'<>&\\", c)) return false;
  }
  memcpy(out, v, n);
  size_t len = n;
  if (!slash) out[len++] = '/';
  out[len] = 0;
  return true;
}

// 设备 ID：MAC 去掉冒号、转小写，例如 84:F3:EB:DD:27:00 → 84f3ebdd2700。不是 12 位十六进制时为空
inline void deviceId(const char* mac, char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
  if (!mac || cap < 13) return;
  size_t n = 0;
  for (const char* p = mac; *p; p++) {
    char c = *p;
    if (c == ':' || c == '-') continue;
    if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')) || n >= 12) {
      out[0] = 0;
      return;
    }
    out[n++] = c;
  }
  out[n == 12 ? n : 0] = 0;
}

// 两个 MAC 是否相同：不区分大小写，冒号、横线可有可无。任一方为空或格式不对都算不同
inline bool sameMac(const char* a, const char* b) {
  char x[13], y[13];
  deviceId(a, x, sizeof(x));
  deviceId(b, y, sizeof(y));
  return x[0] && strcmp(x, y) == 0;
}

enum class Action : uint8_t { None, Start, Emergency, JogForward, JogReverse, Restart };

struct Command {
  char mac[18] = "";
  Action action = Action::None;
  int8_t on = -1;  // 1 = on，0 = off，-1 = 值不认识
};

namespace detail {
inline int8_t onOff(const char* v, JsonScanner::Kind k) {
  typedef JsonScanner::Kind K;
  if (k == K::True) return 1;
  if (k == K::False) return 0;
  if (k != K::String && k != K::Number) return -1;
  if (!strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true")) return 1;
  if (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false")) return 0;
  return -1;
}

inline void onJson(void* ctx, const JsonScanner& js, JsonScanner::Event ev, const char* v, JsonScanner::Kind k) {
  if (ev != JsonScanner::Event::Value || js.depth() != 1) return;  // 只看根对象的直接字段
  Command& c = *static_cast<Command*>(ctx);
  const char* key = js.key(0);
  if (!strcmp(key, "mac")) {
    if (k == JsonScanner::Kind::String) {
      strncpy(c.mac, v, sizeof(c.mac) - 1);
      c.mac[sizeof(c.mac) - 1] = 0;
    }
    return;
  }
  if (c.action != Action::None) return;  // 一条消息只执行第一个动作
  Action a = Action::None;
  if (!strcmp(key, "start")) a = Action::Start;
  else if (!strcmp(key, "emg")) a = Action::Emergency;
  else if (!strcmp(key, "jogf")) a = Action::JogForward;
  else if (!strcmp(key, "jogr")) a = Action::JogReverse;
  else if (!strcmp(key, "restart")) a = Action::Restart;
  if (a == Action::None) return;  // ip 等其它字段忽略
  c.action = a;
  c.on = onOff(v, k);
}
}  // namespace detail

// 解析控制消息。根必须是对象；"mac" 是字符串；动作是 start / emg / jogf / jogr / restart 里的第一个，
// 值可以是 "on"/"off"、true/false、1/0（字符串或数字）。JSON 格式不对返回 false
inline bool parseControl(const char* json, size_t n, Command& out) {
  out = Command();
  JsonScanner js;
  js.reset();
  js.setHandler(detail::onJson, &out);
  bool rootObj = false;
  for (size_t i = 0; i < n; i++) {
    const char ch = json[i];
    if (!rootObj && ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
      if (ch != '{') return false;
      rootObj = true;
    }
    js.feed(ch);
    if (js.error()) return false;
  }
  return js.done();
}

}  // namespace lan
}  // namespace semi
