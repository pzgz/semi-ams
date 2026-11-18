// 扁平 JSON 取值 + MAC 规范化（纯 C++，电脑上能直接测，见 host_test/）
//
// 设备的 state / availability 都是扁平的：{"state":"打印中","key":"PRINTING",...,"mac":"84:f3:eb:dd:27:00"}
// 不需要完整的 JSON 解析器，按 key 找一遍就够了，省内存也不用引 ArduinoJson。
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace semi {
namespace json {

inline const char* skipWs(const char* p) {
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') ++p;
  return p;
}

// 取 key 的值写进 out：字符串去掉引号，数字/布尔原样（"true"/"false"/"12"）。
// 没有这个键、值是嵌套对象/数组、缓冲区不够，都返回 false
inline bool get(const char* js, const char* key, char* out, size_t cap) {
  if (!js || !key || !key[0] || cap < 2) return false;
  const size_t kl = strlen(key);
  const char* p = js;
  while ((p = strchr(p, '"')) != nullptr) {
    ++p;  // 跳过左引号
    if (strncmp(p, key, kl) == 0 && p[kl] == '"') {
      const char* q = skipWs(p + kl + 1);  // 跳过 key 的右引号
      if (*q != ':') {                     // 不是键值对（例如字符串里恰好出现同样的字）
        while (*p && *p != '"') {
          if (*p == '\\' && p[1]) ++p;
          ++p;
        }
        if (!*p) return false;
        ++p;
        continue;
      }
      q = skipWs(q + 1);
      size_t n = 0;
      if (*q == '"') {
        ++q;
        while (*q && *q != '"') {
          char c = *q++;
          if (c == '\\' && *q) c = *q++;  // \" \\ \/ 之类，只去掉反斜杠，不做完整解码
          if (n + 1 < cap) out[n++] = c;
        }
        out[n] = 0;
        return true;
      }
      if (*q == '{' || *q == '[') return false;  // 嵌套的不取
      while (*q && *q != ',' && *q != '}' && *q != ']' && *q != ' ' && *q != '\t' && *q != '\r' &&
             *q != '\n') {
        if (n + 1 < cap) out[n++] = *q;
        ++q;
      }
      out[n] = 0;
      return n > 0;
    }
    // 不是目标键：跳过这整个字符串
    while (*p && *p != '"') {
      if (*p == '\\' && p[1]) ++p;
      ++p;
    }
    if (!*p) return false;
    ++p;
  }
  return false;
}

inline bool getLong(const char* js, const char* key, long* out) {
  char b[24];
  if (!get(js, key, b, sizeof(b))) return false;
  if (!b[0]) return false;
  *out = strtol(b, nullptr, 10);
  return true;
}

// 把字符串转义后写进 JSON（mac、状态名都是可控内容，但材料类型是用户填的）
inline void escapeTo(const char* s, char* out, size_t cap) {
  size_t n = 0;
  for (const char* p = s; p && *p && n + 7 < cap; ++p) {
    unsigned char c = (unsigned char)*p;
    if (c == '"' || c == '\\') {
      out[n++] = '\\';
      out[n++] = (char)c;
    } else if (c < 0x20) {
      n += (size_t)snprintf(out + n, cap - n, "\\u%04x", c);
    } else {
      out[n++] = (char)c;
    }
  }
  out[n] = 0;
}

}  // namespace json

namespace mac {

inline int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// 12 位十六进制（冒号、横线、空格可有可无、大小写都行）→ 12 位小写
inline bool toId(const char* in, char* out, size_t cap) {
  if (!in || cap < 13) return false;
  size_t n = 0;
  for (const char* p = in; *p; ++p) {
    if (*p == ':' || *p == '-' || *p == ' ') continue;
    int v = hexVal(*p);
    if (v < 0) return false;
    if (n < 12) out[n++] = (char)(v < 10 ? '0' + v : 'a' + v - 10);
  }
  out[n < 12 ? n : 12] = 0;
  return n == 12;
}

// → "84:f3:eb:dd:27:00"
inline bool normalize(const char* in, char* out, size_t cap) {
  if (cap < 18) return false;
  char id[13];
  if (!toId(in, id, sizeof(id))) return false;
  snprintf(out, cap, "%c%c:%c%c:%c%c:%c%c:%c%c:%c%c", id[0], id[1], id[2], id[3], id[4], id[5],
           id[6], id[7], id[8], id[9], id[10], id[11]);
  return true;
}

}  // namespace mac
}  // namespace semi
