#include "webui.h"

#include <ESPmDNS.h>
#include <WebServer.h>
#include <WiFi.h>

#include "broker.h"
#include "config.h"
#include "devices.h"
#include "jsonmac.h"
#include "settings.h"
#include "webpage.h"

namespace ctrl {
namespace webui {

namespace {

WebServer server(80);

String esc(const char* s) {
  char buf[192];
  semi::json::escapeTo(s ? s : "", buf, sizeof(buf));
  return String(buf);
}

void sendJson(int code, const String& body) {
  server.sendHeader("Cache-Control", "no-store");
  server.send(code, "application/json; charset=utf-8", body);
}

void sendErr(int code, const String& err) {
  sendJson(code, "{\"ok\":false,\"err\":\"" + esc(err.c_str()) + "\"}");
}

// 本机地址：连上路由器时是路由器分的 IP，配网模式是 192.168.4.1
String selfIp() {
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  if (WiFi.getMode() & WIFI_AP) return WiFi.softAPIP().toString();
  return String();
}

// ---------------- /api/state ----------------

String stateJson() {
  String s;
  s.reserve(2800);
  char buf[128];

  s += "{\"ip\":\"";
  s += esc(selfIp().c_str());
  s += "\",\"wifi\":{\"ssid\":\"";
  s += esc(WiFi.status() == WL_CONNECTED ? WiFi.SSID().c_str() : "");
  s += "\",\"ip\":\"";
  s += esc(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "");
  s += "\",\"rssi\":";
  s += (WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  s += "},";

  s += "\"mqtt\":{\"running\":";
  s += gBroker.running() ? "true" : "false";
  s += ",\"port\":";
  s += gBroker.port();
  s += ",\"clients\":";
  s += gBroker.clientsConnected();
  s += ",\"links\":";
  s += gBroker.clientsTotal();
  s += ",\"cap\":";
  s += gBroker.clientsCapacity();
  s += ",\"status\":\"";
  s += esc(gBroker.status().c_str());
  s += "\",\"prefix\":\"";
  s += esc(gSettings.prefix);
  s += "\",\"user\":\"";
  s += esc(gSettings.user);
  s += "\",\"in\":";
  s += gBroker.inMessages();
  s += ",\"out\":";
  s += gBroker.outMessages();
  s += "},";

  snprintf(buf, sizeof(buf), "\"fw\":{\"name\":\"%s\",\"version\":\"%s\",\"heap\":%u,\"uptime\":%lu},",
           FW_NAME, FW_VERSION, (unsigned)ESP.getFreeHeap(), (unsigned long)(millis() / 1000));
  s += buf;

  s += "\"devices_used\":";
  s += devicesUsed();
  s += ",\"devices_online\":";
  s += devicesOnline();
  s += ",\"devices\":[";

  const uint32_t now = millis();
  const int used = devicesUsed();
  for (int i = 0; i < used; ++i) {
    const Device& d = deviceAt(i);
    if (i) s += ",";
    s += "{\"i\":";
    s += i;
    s += ",\"mac\":\"";
    s += esc(d.mac);
    s += "\",\"ip\":\"";
    s += esc(d.ip);
    s += "\",\"online\":";
    s += d.online && d.mac[0] ? "true" : "false";
    s += ",\"state\":\"";
    s += esc(d.state);
    s += "\",\"key\":\"";
    s += esc(d.key);
    s += "\",\"motor\":\"";
    s += esc(d.motor);
    snprintf(buf, sizeof(buf),
             "\",\"duty\":%d,\"in\":%d,\"ready\":%d,\"lack\":%d,\"full\":%d,\"rssi\":%ld,\"heap\":%ld,"
             "\"uptime\":%ld,\"age\":%ld,\"color\":\"",
             d.duty, d.in, d.ready, d.lack, d.full, d.rssi, d.heap, d.uptime,
             d.lastSeenMs ? (long)((now - d.lastSeenMs) / 1000) : -1L);
    s += buf;
    s += esc(d.color);
    s += "\",\"material\":\"";
    s += esc(d.material);
    s += "\"}";
  }
  s += "]}";
  return s;
}

// ---------------- POST 接口 ----------------

// 网页提交设备时优先带 mac（行号会被删除操作往前挪，按行号可能改到隔壁那台），
// 没带 mac 就退回 idx，老脚本照旧能用。kIdxGone = mac 已经不在表里。
const int kIdxGone = -2;

int idxFromRequest(String& err) {
  const String mac = server.arg("mac");
  if (!mac.length()) return server.arg("idx").toInt();
  const int i = devicesFindByMacId(mac.c_str());
  if (i < 0) {
    err = "这个 mac 已经不在设备表里了（可能刚被删掉），刷新一下页面";
    return kIdxGone;
  }
  return i;
}

void handleConfig() {
  const String oldPrefix = gSettings.prefix;
  String err;
  if (!settingsSetServer(server.arg("port").c_str(), server.arg("user").c_str(),
                         server.arg("pass").c_str(), server.arg("prefix").c_str(), err)) {
    sendErr(400, err);
    return;
  }
  Serial.printf("[网页] 服务端设置：端口 %u，前缀 %s，%s\n", gSettings.brokerPort, gSettings.prefix,
                gSettings.user[0] ? "要登录" : "允许匿名");
  // 端口变了要重开监听；改了账号对已经连上的客户端不追溯，重连时才校验
  gBroker.begin();
  if (oldPrefix != gSettings.prefix) {
    Serial.println("[网页] 前缀变了，进料器上的 topic 前缀也要一起改");
  }
  // 设置已经存了，但监听可能没起来（端口被占），把结果一起回给网页
  sendJson(200, "{\"ok\":true,\"running\":" + String(gBroker.running() ? "true" : "false") +
                    ",\"status\":\"" + esc(gBroker.status().c_str()) + "\",\"prefix\":\"" +
                    esc(gSettings.prefix) + "\",\"port\":" + String(gSettings.brokerPort) + "}");
}

void handleLabel() {
  String err;
  const int idx = idxFromRequest(err);
  if (idx == kIdxGone) {
    sendErr(409, err);
    return;
  }
  if (!devicesSetLabel(idx, server.arg("color").c_str(), server.arg("material").c_str(), err)) {
    sendErr(400, err);
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleAddDevice() {
  int idx = -1;
  String err;
  if (!devicesAdd(server.arg("mac").c_str(), server.arg("color").c_str(),
                  server.arg("material").c_str(), idx, err)) {
    sendErr(400, err);
    return;
  }
  sendJson(200, "{\"ok\":true,\"idx\":" + String(idx) + "}");
}

void handleDelDevice() {
  String err;
  const int idx = idxFromRequest(err);
  if (idx == kIdxGone) {
    sendErr(409, err);
    return;
  }
  if (!devicesRemove(idx, err)) {
    sendErr(400, err);
    return;
  }
  sendJson(200, "{\"ok\":true}");
}

void handleControl() {
  String reqErr;
  const int idx = idxFromRequest(reqErr);
  if (idx == kIdxGone) {
    sendErr(409, reqErr);
    return;
  }
  const String cmd = server.arg("cmd");
  // 动作名先校验：参数写错就该 400，跟这一行有没有设备、在不在线无关
  if (cmd != "feed" && cmd != "retract" && cmd != "emg" && cmd != "emgclear" && cmd != "jogf" &&
      cmd != "jogr" && cmd != "restart") {
    sendErr(400, "不认识的动作");
    return;
  }
  if (idx < 0 || idx >= devicesUsed()) {
    sendErr(400, "槽位序号不对");
    return;
  }
  const Device& d = deviceAt(idx);
  if (!d.mac[0]) {
    sendErr(400, "这一行还没有设备");
    return;
  }
  // 设备离线就别假装发了：它现在没连着 MQTT，命令只会掉进空里
  if (!d.online) {
    String e = "「" + String(d.mac) + "」离线";
    if (d.lastSeenMs) {
      e += "（最后一条消息 ";
      e += (long)((millis() - d.lastSeenMs) / 1000);
      e += " 秒前）";
    }
    e += "，命令没有发出去";
    sendErr(409, e);
    return;
  }

  // 界面上的按钮 → 文档里的动作（见 firmware/docs/局域网MQTT消息参考.md 第 5 节）
  const char* action = nullptr;
  const char* value = "on";
  if (cmd == "feed") {
    action = "start";
    value = "on";
  } else if (cmd == "retract") {
    action = "start";
    value = "off";
  } else if (cmd == "emg") {
    action = "emg";
    value = "on";
  } else if (cmd == "emgclear") {
    action = "emg";
    value = "off";
  } else if (cmd == "jogf") {
    action = "jogf";
  } else if (cmd == "jogr") {
    action = "jogr";
  } else if (cmd == "restart") {
    action = "restart";
  } else {
    sendErr(400, "不认识的动作");
    return;
  }

  char payload[96];
  snprintf(payload, sizeof(payload), "{\"mac\":\"%s\",\"%s\":\"%s\"}", d.mac, action, value);
  char topic[MQTT_TOPIC_MAX];
  snprintf(topic, sizeof(topic), "%scontrol", gSettings.prefix);

  String err;
  int sent = 0;
  if (!gBroker.publish(topic, payload, strlen(payload), false, err, &sent)) {
    sendErr(503, err);
    return;
  }
  Serial.printf("[网页] %s ← %s=%s（送达 %d 台）\n", d.mac, action, value, sent);
  sendJson(200, "{\"ok\":true,\"sent\":" + String(sent) + "}");
}

// ---------------- 配网页面 ----------------

String ssidOptions() {
  String html;
  const int n = WiFi.scanNetworks();
  for (int i = 0; i < n && i < 20; ++i) {
    String s = WiFi.SSID(i);
    if (!s.length()) continue;
    if (html.indexOf("\"" + s + "\"") >= 0) continue;  // 同名多个 AP 只列一次
    html += "<option value=\"" + s + "\">";
  }
  WiFi.scanDelete();
  return html;
}

void handlePortalIndex() {
  String page((const char*)PAGE_PORTAL);
  page.replace("%SSIDS%", ssidOptions());
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "text/html; charset=utf-8", page);
}

void handleWifiSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (!ssid.length()) {
    server.send(400, "text/html; charset=utf-8",
                "<meta charset=utf-8><p>WiFi 名称不能为空，<a href=/>返回</a></p>");
    return;
  }
  wifiSave(ssid, pass);
  server.send(200, "text/html; charset=utf-8",
              "<meta charset=utf-8><p>已保存，设备正在重启并连接 " + ssid +
                  "。<br>连上以后用 http://" CTRL_HOSTNAME ".local 或路由器里查到的 IP 打开控制页面。</p>");
  delay(600);
  ESP.restart();
}

void handleIndex() {
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "text/html; charset=utf-8", PAGE_INDEX);
}

void handleNotFound() { server.send(404, "text/plain; charset=utf-8", "not found"); }

}  // namespace

void begin(bool portal) {
  if (portal) {
    server.on("/", HTTP_GET, handlePortalIndex);
    server.on("/wifi", HTTP_POST, handleWifiSave);
  } else {
    server.on("/", HTTP_GET, handleIndex);
    server.on("/api/state", HTTP_GET, []() { sendJson(200, stateJson()); });
    server.on("/api/config", HTTP_POST, handleConfig);
    server.on("/api/label", HTTP_POST, handleLabel);
    server.on("/api/control", HTTP_POST, handleControl);
    server.on("/api/adddev", HTTP_POST, handleAddDevice);
    server.on("/api/deldev", HTTP_POST, handleDelDevice);
  }
  server.onNotFound(handleNotFound);
  server.begin();
  Serial.printf("[网页] 已启动%s\n", portal ? "（配网模式）" : "");
}

void loop() { server.handleClient(); }

}  // namespace webui
}  // namespace ctrl
