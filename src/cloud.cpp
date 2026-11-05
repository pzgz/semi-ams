// 点灯 Blinker：控件名称与原厂"app控制界面配置.txt"一致，直接沿用原来的 App 界面
//   btn-start-stop  开始/结束        btn-emergency  紧急停止/解除
//   btn-restart     重启              btn-mqtt       开启/关闭拓竹打印机 MQTT（按钮文字显示连接状态）
//   tex-state       工作状态          tex-motor      电机状态
//   tex-runtime     系统运行时长      num-rssi       WiFi 信号
//   tex-ip          IP                tex-hint       提示
#define BLINKER_WIFI
// #define BLINKER_PRINT Serial   // 需要看点灯库的调试信息时打开
#include <Blinker.h>

#include "app.h"
#include "cloud.h"
#include "config.h"
#include "fsm.h"
#include "printer.h"
#include "settings.h"

namespace {

char gAuth[40];
bool gStarted = false;
uint32_t gStartedAt = 0;
bool gInitDone = false;  // 点灯已完成第一次连接后的初始化
uint32_t gLastPush = 0;
uint32_t gLastRev = 0;
bool gConnected = false;
uint32_t gLastConnCheck = 0;
uint32_t gLastMqttRev = 0;

BlinkerButton btnStartStop((char*)"btn-start-stop");
BlinkerButton btnEmergency((char*)"btn-emergency");
BlinkerButton btnRestart((char*)"btn-restart");
BlinkerButton btnMqtt((char*)"btn-mqtt");
BlinkerText texState((char*)"tex-state");
BlinkerText texMotor((char*)"tex-motor");
BlinkerText texRuntime((char*)"tex-runtime");
BlinkerText texIp((char*)"tex-ip");
BlinkerText texHint((char*)"tex-hint");
BlinkerNumber numRssi((char*)"num-rssi");

bool isTap(const String& s) { return s == BLINKER_CMD_BUTTON_TAP; }

void onStartStop(const String& s) {
  if (isTap(s)) app::requestStartStop(PSTR("App"));
}
void onEmergency(const String& s) {
  if (isTap(s)) app::requestEmergency(PSTR("App"));
}
void onRestart(const String& s) {
  if (isTap(s)) app::requestRestart(PSTR("App"));
}
void pushMqtt() {
  const char* text;
  const char* color;
  if (!settingsMqttConfigured()) {
    text = PSTR("MQTT未设置");
    color = "#595959";
  } else if (!printer::enabled()) {
    text = PSTR("MQTT已关闭");
    color = "#595959";
  } else if (printer::connected()) {
    text = PSTR("MQTT已连接");
    color = "#00A90C";
  } else {
    text = PSTR("MQTT连接中");
    color = "#FBA613";
  }
  btnMqtt.text(String(FPSTR(text)));  // text 在 Flash 里
  btnMqtt.color(color);
  btnMqtt.print();
  gLastMqttRev = printer::revision();
}

void onMqtt(const String& s) {
  if (!isTap(s)) return;
  if (!settingsMqttConfigured()) {
    texHint.print(String(F("先在网页填写打印机IP、序列号和访问码：http://")) + WiFi.localIP().toString());
  } else {
    printer::setEnabled(!printer::enabled());
    texHint.print(String(printer::enabled() ? F("拓竹打印机MQTT已开启") : F("拓竹打印机MQTT已关闭")));
  }
  pushMqtt();
}

String motorText() {
  app::Status s = app::status();
  return String(FPSTR(semi::Fsm::motorName(s.motor)));
}

void pushState() {
  texState.print(app::stateText());
  texMotor.print(motorText());
}

// App 打开设备页面或定期心跳时，刷新全部控件（和心跳回复合并成一条消息发送）
void heartbeat() {
  pushState();
  texRuntime.print(app::uptimeText());
  numRssi.print(WiFi.RSSI());
  texIp.print(WiFi.localIP().toString());
  texHint.print(String(F("网页调参：http://")) + WiFi.localIP().toString());
  pushMqtt();
  gLastPush = millis();
  gLastRev = app::status().rev;
}

}  // namespace

namespace cloud {

void begin(const char* authKey) {
  strlcpy(gAuth, authKey, sizeof(gAuth));
  static String ssid, psk;
  ssid = WiFi.SSID();
  psk = WiFi.psk();
  Blinker.begin(gAuth, ssid.c_str(), psk.c_str());
  btnStartStop.attach(onStartStop);
  btnEmergency.attach(onEmergency);
  btnRestart.attach(onRestart);
  btnMqtt.attach(onMqtt);
  Blinker.attachHeartbeat(heartbeat);
  gStarted = true;
  gStartedAt = millis();
  LOG("点灯 Blinker 已启动");
}

void loop() {
  if (!gStarted) return;
  Blinker.run();
  if (!gInitDone && Blinker.init()) {
    gInitDone = true;
    LOG("点灯初始化完成（空闲内存 %u，最大连续 %u）", (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
  }
  // Blinker.connected() 未连上时会触发连接流程，可能阻塞，所以每 10 秒查一次并缓存
  if (millis() - gLastConnCheck > 10000) {
    gLastConnCheck = millis();
    gConnected = WiFi.status() == WL_CONNECTED && Blinker.connected();
  }
  // 状态变化时主动推送（点灯限制设备每秒最多发 1 条，这里至少间隔 1.5 秒，多次变化合并发送）
  uint32_t rev = app::status().rev;
  if (rev != gLastRev && millis() - gLastPush >= UI_PUSH_INTERVAL_MS) {
    gLastRev = rev;
    gLastPush = millis();
    pushState();
  } else if (printer::revision() != gLastMqttRev && millis() - gLastPush >= UI_PUSH_INTERVAL_MS) {
    gLastPush = millis();
    pushMqtt();
  }
}

bool started() { return gStarted; }

bool settled() {
  if (!gSettings.blinkerKey[0]) return true;  // 不用点灯
  if (!gStarted) return false;                 // 还没启动（WiFi 还没连上）
  return gInitDone || millis() - gStartedAt > CLOUD_SETTLE_MAX_MS;
}

const char* statusText() {
  if (!gStarted) return PSTR("未启用");
  return gConnected ? PSTR("已连接") : PSTR("连接中");
}

void showRestarting() {
  if (!gStarted) return;
  texState.print(String(F("系统重启中...")));
  texMotor.print(String(F("停止")));
  Blinker.delay(1200);  // 等消息发出
}

}  // namespace cloud
