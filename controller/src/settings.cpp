#include "settings.h"

#include <Preferences.h>

#include "config.h"

namespace ctrl {

Settings gSettings;

namespace {

const char* kDefaultPrefix = "semiams/";

bool isPrintableAscii(const char* s, size_t maxLen) {
  size_t n = strlen(s);
  if (n > maxLen) return false;
  for (size_t i = 0; i < n; ++i) {
    char c = s[i];
    if (c < 0x20 || c > 0x7E) return false;
    if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' || c == '\\') return false;
  }
  return true;
}

}  // namespace

void settingsBegin() {
  Preferences p;
  p.begin("cfg", true);
  gSettings.brokerPort = p.getUShort("port", MQTT_PORT_DEFAULT);
  String user = p.getString("user", "");
  String pass = p.getString("pass", "");
  String prefix = p.getString("prefix", kDefaultPrefix);
  p.end();

  strncpy(gSettings.user, user.c_str(), sizeof(gSettings.user) - 1);
  gSettings.user[sizeof(gSettings.user) - 1] = 0;
  strncpy(gSettings.pass, pass.c_str(), sizeof(gSettings.pass) - 1);
  gSettings.pass[sizeof(gSettings.pass) - 1] = 0;
  strncpy(gSettings.prefix, prefix.c_str(), sizeof(gSettings.prefix) - 1);
  gSettings.prefix[sizeof(gSettings.prefix) - 1] = 0;
  if (!gSettings.prefix[0]) strcpy(gSettings.prefix, kDefaultPrefix);
  if (!gSettings.brokerPort) gSettings.brokerPort = MQTT_PORT_DEFAULT;
}

void settingsSave() {
  Preferences p;
  p.begin("cfg", false);
  p.putUShort("port", gSettings.brokerPort);
  p.putString("user", gSettings.user);
  p.putString("pass", gSettings.pass);
  p.putString("prefix", gSettings.prefix);
  p.end();
}

bool settingsSetServer(const char* port, const char* user, const char* pass, const char* prefix,
                       String& err) {
  String sPort = port ? String(port) : String();
  sPort.trim();
  String sUser = user ? String(user) : String();
  sUser.trim();
  String sPrefix = prefix ? String(prefix) : String();
  sPrefix.trim();

  long p = sPort.length() ? sPort.toInt() : MQTT_PORT_DEFAULT;
  if (p < 1 || p > 65535) {
    err = "端口要在 1–65535 之间";
    return false;
  }
  if (p == 80) {
    err = "80 端口被网页占了，换个端口";
    return false;
  }
  if (!isPrintableAscii(sUser.c_str(), 32)) {
    err = "用户名最长 32 位，只能是可打印 ASCII，不能有 \" ' < > & \\";
    return false;
  }
  if (pass && !isPrintableAscii(pass, 64)) {
    err = "密码最长 64 位，只能是可打印 ASCII，不能有 \" ' < > & \\";
    return false;
  }
  if (!sPrefix.length()) sPrefix = kDefaultPrefix;  // 留空恢复默认
  if (sPrefix.length() > 32) {
    err = "topic 前缀最长 32 字节";
    return false;
  }
  if (sPrefix.indexOf('+') >= 0 || sPrefix.indexOf('#') >= 0 || sPrefix.indexOf(' ') >= 0) {
    err = "topic 前缀不能有 + # 和空格";
    return false;
  }
  for (size_t i = 0; i < sPrefix.length(); ++i) {
    char c = sPrefix[i];
    if ((unsigned char)c < 0x20) {
      err = "topic 前缀不能有控制字符";
      return false;
    }
    if (c == '"' || c == '\'' || c == '<' || c == '>' || c == '&' || c == '\\') {
      err = "topic 前缀不能有 \" ' < > & \\";
      return false;
    }
  }
  if (!sPrefix.endsWith("/")) sPrefix += "/";  // 和固件一样自动补

  gSettings.brokerPort = (uint16_t)p;
  strncpy(gSettings.user, sUser.c_str(), sizeof(gSettings.user) - 1);
  gSettings.user[sizeof(gSettings.user) - 1] = 0;
  // 密码留空表示不修改；清空用户名时密码一起清掉（和固件的规则一致）
  if (pass && pass[0]) {
    strncpy(gSettings.pass, pass, sizeof(gSettings.pass) - 1);
    gSettings.pass[sizeof(gSettings.pass) - 1] = 0;
  } else if (!gSettings.user[0]) {
    gSettings.pass[0] = 0;
  }
  strncpy(gSettings.prefix, sPrefix.c_str(), sizeof(gSettings.prefix) - 1);
  gSettings.prefix[sizeof(gSettings.prefix) - 1] = 0;
  settingsSave();
  return true;
}

bool wifiLoad(String& ssid, String& pass) {
  Preferences p;
  p.begin("wifi", true);
  ssid = p.getString("ssid", "");
  pass = p.getString("pass", "");
  p.end();
  return ssid.length() > 0;
}

void wifiSave(const String& ssid, const String& pass) {
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
}

void wifiClear() {
  Preferences p;
  p.begin("wifi", false);
  p.clear();
  p.end();
}

}  // namespace ctrl
