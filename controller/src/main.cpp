// SEMI-AMS 主控（ESP32-C3）
//
// 本机就是 MQTT 服务端（broker）：进料器的"局域网 MQTT"里填本机 IP 和端口，
// 直接连上来。本机收下每台的 state / availability 排成表格显示在网页上，
// 按钮往 <前缀>control 发命令（payload 里带目标设备的 mac）。
// 进料器那边的协议见 firmware/docs/局域网MQTT消息参考.md。
#include <Arduino.h>
#include <ESPmDNS.h>
#include <WiFi.h>

#include "broker.h"
#include "config.h"
#include "devices.h"
#include "settings.h"
#include "webui.h"

using namespace ctrl;  // 这个工程只有一个命名空间，文件里就不再逐个写 ctrl::

namespace {

bool gPortal = false;         // 当前跑在配网热点模式（STA 连不上才会这样）
uint32_t gLostSince = 0;      // 第一次发现掉线的 millis，0 = 现在连着
uint32_t gLastRetry = 0;      // 上一次催重连的时刻

void startPortal(const char* why) {
  Serial.printf("[WiFi] %s，开配网热点 %s（http://192.168.4.1）\n", why, CTRL_AP_NAME);
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAP(CTRL_AP_NAME);  // 开放热点，和进料器的配网热点一致
  gPortal = true;
  webui::begin(true);
}

void startSta() {
  String ssid, pass;
  if (!wifiLoad(ssid, pass)) {
    startPortal("没有保存 WiFi");
    return;
  }
  Serial.printf("[WiFi] 连接 %s …\n", ssid.c_str());
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(CTRL_HOSTNAME);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.c_str());

  const uint32_t deadline = millis() + WIFI_CONNECT_TIMEOUT_S * 1000UL;
  while (millis() < deadline) {
    if (WiFi.status() == WL_CONNECTED) {
      gPortal = false;
      gLostSince = 0;
      Serial.printf("[WiFi] 已连接，IP %s，信号 %d dBm\n", WiFi.localIP().toString().c_str(),
                    WiFi.RSSI());
      Serial.printf("[WiFi] 进料器的 MQTT 服务器就填 %s\n", WiFi.localIP().toString().c_str());
      if (MDNS.begin(CTRL_HOSTNAME)) {
        MDNS.addService("http", "tcp", 80);
        Serial.printf("[WiFi] http://%s.local\n", CTRL_HOSTNAME);
      }
      webui::begin(false);
      gBroker.begin();
      return;
    }
    delay(200);
  }
  startPortal("连不上保存的 WiFi");
}

// 运行中掉线不能像原来那样干等：自动重连失败次数用完之后就再也不试了，
// 换路由器、改密码之后设备会永久失联（网页也打不开，只能重新烧录）。
// 这里每 10 秒催一次，连续 3 分钟回不来就重启——重启后连不上会自动开配网热点。
void wifiGuard() {
  if (gPortal) return;  // 配网模式没有 STA 可守
  const uint32_t now = millis();
  if (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
    if (gLostSince) Serial.printf("[WiFi] 恢复了，IP %s\n", WiFi.localIP().toString().c_str());
    gLostSince = 0;
    return;
  }
  if (!gLostSince) {
    gLostSince = now;
    gLastRetry = 0;
    Serial.println("[WiFi] 掉线了，开始每 10 秒重连一次");
    return;
  }
  const uint32_t lost = now - gLostSince;
  if (lost >= WIFI_RETRY_INTERVAL_MS && (now - gLastRetry) >= WIFI_RETRY_INTERVAL_MS) {
    gLastRetry = now;
    Serial.printf("[WiFi] 掉线 %lu 秒，重连一次\n", (unsigned long)(lost / 1000));
    WiFi.disconnect();
    WiFi.reconnect();
  }
  if (lost >= WIFI_LOST_RESTART_MS) {
    Serial.printf("[WiFi] 掉线 %lu 秒还没回来，重启（下次开机连不上会开 %s 热点）\n",
                  (unsigned long)(lost / 1000), CTRL_AP_NAME);
    delay(100);
    ESP.restart();
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\n%s %s 启动\n", FW_NAME, FW_VERSION);

  settingsBegin();
  devicesBegin();
  Serial.printf("[设置] MQTT 服务端端口 %u，前缀 %s，%s；设备表 %d 台\n", gSettings.brokerPort,
                gSettings.prefix, gSettings.user[0] ? "要登录" : "允许匿名", devicesUsed());

  startSta();
}

void loop() {
  webui::loop();
  gBroker.loop();
  devicesLoop();
  wifiGuard();
  static uint32_t lastLog = 0;
  if (millis() - lastLog > 60000) {
    lastLog = millis();
    Serial.printf("[状态] WiFi %s，%s，在线 %d/%d 台，空闲内存 %u\n",
                  WiFi.status() == WL_CONNECTED ? "已连接" : "未连接", gBroker.status().c_str(),
                  devicesOnline(), devicesUsed(), (unsigned)ESP.getFreeHeap());
  }
}
