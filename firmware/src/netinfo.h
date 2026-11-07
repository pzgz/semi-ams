// 设备网络身份：当前 IP 和芯片 MAC。
// 取不到就输出空串（未联网、地址为 0.0.0.0、MAC 全 0 都算取不到），
// 调用方可以直接塞进 JSON 或网页，不用各自判断。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#if defined(ARDUINO)
#include <ESP8266WiFi.h>
#endif

namespace netinfo {

inline void ip(char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
#if defined(ARDUINO)
  if (WiFi.status() != WL_CONNECTED) return;
  const IPAddress a = WiFi.localIP();
  if (!(a[0] || a[1] || a[2] || a[3])) return;
  snprintf(out, cap, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
#endif
}

inline void mac(char* out, size_t cap) {
  if (!out || !cap) return;
  out[0] = 0;
#if defined(ARDUINO)
  uint8_t m[6] = {};
  if (!WiFi.macAddress(m)) return;
  bool any = false;
  for (uint8_t i = 0; i < 6; i++) {
    if (m[i]) {
      any = true;
      break;
    }
  }
  if (!any) return;  // 全 0 也算没取到
  snprintf(out, cap, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
#endif
}

}  // namespace netinfo
