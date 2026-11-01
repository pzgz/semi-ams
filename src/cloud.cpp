// 点灯 Blinker：控件名称与原厂"app控制界面配置.txt"一致，直接沿用原来的 App 界面
//   btn-start-stop  开始/结束        btn-emergency  紧急停止/解除
//   btn-restart     重启              btn-mqtt       （本固件不支持 MQTT，显示为灰色）
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

namespace {

char gAuth[40];
bool gStarted = false;
uint32_t gLastPush = 0;
uint32_t gLastRev = 0;
bool gConnected = false;
uint32_t gLastConnCheck = 0;

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
  if (isTap(s)) app::requestStartStop("App");
}
void onEmergency(const String& s) {
  if (isTap(s)) app::requestEmergency("App");
}
void onRestart(const String& s) {
  if (isTap(s)) app::requestRestart("App");
}
void onMqtt(const String& s) {
  if (isTap(s)) texHint.print("本固件不支持MQTT");
}

String motorText() {
  app::Status s = app::status();
  return semi::Fsm::motorName(s.motor);
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
  texHint.print(String("网页调参：http://") + WiFi.localIP().toString());
  btnMqtt.text("MQTT未启用");
  btnMqtt.color("#595959");
  btnMqtt.print();
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
  app::log("点灯 Blinker 已启动");
}

void loop() {
  if (!gStarted) return;
  Blinker.run();
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
  }
}

bool started() { return gStarted; }

const char* statusText() {
  if (!gStarted) return "未启用";
  return gConnected ? "已连接" : "连接中";
}

void showRestarting() {
  if (!gStarted) return;
  texState.print("系统重启中...");
  texMotor.print("停止");
  Blinker.delay(1200);  // 等消息发出
}

}  // namespace cloud
