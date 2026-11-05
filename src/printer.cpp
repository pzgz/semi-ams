#include "printer.h"

#include <ESP8266WiFi.h>
#include <Updater.h>
#include <WiFiClientSecure.h>

#include "app.h"
#include "bambu.h"
#include "cloud.h"
#include "mqtt_codec.h"
#include "net.h"
#include "pgm.h"
#include "settings.h"

namespace {

const uint16_t kPort = 8883;
const uint16_t kKeepAliveS = 60;
const uint32_t kPingEveryMs = 30000;
const uint32_t kSilenceMs = 75000;     // 这么久没收到任何数据就认为连接已断
const uint32_t kReplyTimeoutMs = 10000;
const uint32_t kBackoffMinMs = 5000;
const uint32_t kBackoffMaxMs = 60000;
const uint32_t kLowHeap = 3000;        // 连接中空闲内存低于这个值就先断开
const uint32_t kAuthBackoffMs = 300000;  // 访问码错误：5 分钟后再试，避免一直重试

enum class Phase : uint8_t { Idle, WaitConnAck, WaitSubAck, Ready };

// 连接时才创建、断开就释放：WiFiClientSecure 一创建就会占用 BearSSL 的独立栈（约 6KB），
// 不用 MQTT 时不应该一直占着
BearSSL::WiFiClientSecure* gTls = nullptr;
semi::BambuMonitor gMon;
semi::mqtt::Reader gReader;
Phase gPhase = Phase::Idle;
uint32_t gPhaseSince = 0;
uint32_t gNextTry = 0;
uint32_t gBackoff = kBackoffMinMs;
uint32_t gLastRx = 0, gLastPing = 0;
uint32_t gRev = 0;
bool gPushallSent = false, gNoPushall = false;
uint32_t gPushallAt = 0;
bool gWaitCloud = false;  // 正在等点灯初始化完成
bool gEverPushall = false;  // 至少发过一次 pushall（用来限制频率）
int gMfln = -1;  // 打印机是否支持 TLS 最大分片协商：-1 未检测，0 不支持，1 支持
char gMflnIp[16] = "";
char gTopic[48] = "";
char gErr[64] = "";
char gEvent[64] = "";
bool gWasEnabled = false;

void bump() { gRev++; }

void setPhase(Phase p) {
  gPhase = p;
  gPhaseSince = millis();
  bump();
}

void monLog(const char* m) { LOG("%s", m); }

// why 可以是 Flash 字符串（PSTR）或内存里的缓冲
void disconnect(const char* why, uint32_t retryInMs = 0);

void send(const uint8_t* buf, size_t n) {
  if (!n || !gTls || gPhase == Phase::Idle) return;
  if (gTls->write(buf, n) != n) disconnect(PSTR("发送失败"));
}

void disconnect(const char* why, uint32_t retryInMs) {
  gReader.stop();  // 可能是在解析回调里断开的：剩下的数据不再处理
  if (gPhase != Phase::Idle) {
    // 刚发完 pushall 就被断开：打印机不允许发送请求，以后不再发
    if (gPushallSent && millis() - gPushallAt < 3000 && !gNoPushall) {
      gNoPushall = true;
      LOG("打印机在收到状态请求后断开了连接，之后不再发送请求，只接收消息");
    }
    uint8_t b[2];
    if (gTls && gTls->connected()) gTls->write(b, semi::mqtt::buildDisconnect(b, sizeof(b)));
    LOG("打印机 MQTT 断开：" SEMI_PS, why);
  }
  if (gTls) {
    gTls->stop();
    delete gTls;
    gTls = nullptr;
  }
  semi_copy_P(gErr, why, sizeof(gErr));  // why 可能在 Flash 里
  gPushallSent = false;
  if (!retryInMs) {
    retryInMs = gBackoff;
    gBackoff = gBackoff * 2 > kBackoffMaxMs ? kBackoffMaxMs : gBackoff * 2;
  }
  gNextTry = millis() + retryInMs;
  setPhase(Phase::Idle);
}

class Listener : public semi::mqtt::Listener {
 public:
  void onConnAck(uint8_t rc) override {
    if (rc != 0) {
      const char* why;
      switch (rc) {
        case 1: why = PSTR("打印机不支持这个 MQTT 版本"); break;
        case 2: why = PSTR("客户端 ID 被拒绝"); break;
        case 3: why = PSTR("打印机 MQTT 服务不可用"); break;
        case 4: why = PSTR("访问码错误"); break;
        case 5: why = PSTR("打印机拒绝连接（访问码错误？）"); break;
        default: why = PSTR("打印机拒绝连接"); break;
      }
      disconnect(why, (rc == 4 || rc == 5) ? kAuthBackoffMs : 0);
      return;
    }
    snprintf(gTopic, sizeof(gTopic), "device/%s/report", gSettings.mqttSn);
    uint8_t buf[80];
    send(buf, semi::mqtt::buildSubscribe(buf, sizeof(buf), 1, gTopic, 0));
    setPhase(Phase::WaitSubAck);
  }

  void onSubAck(uint16_t, uint8_t result) override {
    if (result == 0x80) {
      disconnect(PSTR("订阅被拒绝（序列号是否正确？）"), kAuthBackoffMs);
      return;
    }
    gBackoff = kBackoffMinMs;
    gErr[0] = 0;
    gMon.reset();
    setPhase(Phase::Ready);
    LOG("已连接拓竹打印机 %s", gSettings.mqttIp);
    // 请求一次完整状态（P 系列平时只发送有变化的字段）。拓竹建议不要频繁请求：断线重连时 5 分钟内只发一次
    if (!gNoPushall && (!gEverPushall || millis() - gPushallAt > 300000UL)) {
      gEverPushall = true;
      char topic[48];
      snprintf(topic, sizeof(topic), "device/%s/request", gSettings.mqttSn);
      static const char kPushall[] =
          "{\"pushing\":{\"sequence_id\":\"0\",\"command\":\"pushall\",\"version\":1,\"push_target\":1}}";
      uint8_t buf[160];
      send(buf, semi::mqtt::buildPublish(buf, sizeof(buf), topic, kPushall, sizeof(kPushall) - 1));
      gPushallSent = true;
      gPushallAt = millis();
    }
  }

  void onPingResp() override {}

  void onPublishBegin(const char*, size_t) override { gMon.beginMessage(); }

  void onPublishData(const uint8_t* d, size_t n) override { gMon.feed((const char*)d, n); }

  void onPublishEnd(uint8_t qos, uint16_t packetId) override {
    gMon.endMessage(millis());
    if (qos == 1) {  // 订阅用的是 QoS 0，正常不会出现
      uint8_t buf[4];
      send(buf, semi::mqtt::buildPubAck(buf, sizeof(buf), packetId));
    }
  }
};
Listener gListener;

void connectNow() {
  IPAddress ip;
  if (!ip.fromString(gSettings.mqttIp)) {
    disconnect(PSTR("打印机 IP 格式不对"), kAuthBackoffMs);
    return;
  }
  // 先用普通 TCP 试一下能不能连上（2 秒超时）。打印机关机时 TLS 连接要等 15 秒才放弃，
  // 这期间网页和点灯都没反应；分片协商的检测结果也只在打印机在线时才可信
  {
    WiFiClient tcp;
    tcp.setTimeout(2000);
    if (!tcp.connect(ip, kPort)) {
      disconnect(PSTR("连不上打印机（IP 是否正确？打印机是否开机？）"));
      return;
    }
    tcp.stop();
  }
  // 打印机支持 TLS 最大分片协商时，接收缓冲只要 1KB；否则要 16KB（一条 TLS 记录的最大长度）
  if (gMfln < 0 || strcmp(gMflnIp, gSettings.mqttIp) != 0) {
    gMfln = BearSSL::WiFiClientSecure::probeMaxFragmentLength(ip, kPort, 1024) ? 1 : 0;
    strlcpy(gMflnIp, gSettings.mqttIp, sizeof(gMflnIp));
    LOG("打印机" SEMI_PS " TLS 分片协商，接收缓冲 %s", gMfln ? PSTR("支持") : PSTR("不支持"), gMfln ? "1KB" : "16KB");
  }
  const int rx = gMfln ? 1024 : 16384;
  const int tx = gMfln ? 1024 : 512;
  // 除了收发缓冲，BearSSL 还要约 6KB 的独立栈和几 KB 的会话状态
  const uint32_t heap = ESP.getFreeHeap(), block = ESP.getMaxFreeBlockSize();
  if (block < (uint32_t)rx + 512 || heap < (uint32_t)(rx + tx) + 12000) {
    char why[64];
    snprintf_P(why, sizeof(why), PSTR("内存不足（空闲 %u，最大连续 %u）"), (unsigned)heap, (unsigned)block);
    LOG("无法连接拓竹打印机：%s", why);
    gMfln = -1;  // 下次重新检测
    disconnect(why, kBackoffMaxMs);
    return;
  }
  LOG("连接拓竹打印机 %s ...（空闲内存 %u，最大连续 %u）", gSettings.mqttIp, (unsigned)heap, (unsigned)block);
  gTls = new BearSSL::WiFiClientSecure();
  gTls->setInsecure();  // 打印机用自签名证书；只在局域网内连接
  gTls->setBufferSizes(rx, tx);
  const uint32_t t0 = millis();
  if (!gTls->connect(ip, kPort)) {
    char err[48] = "";
    const int code = gTls->getLastSSLError(err, sizeof(err));
    char why[64];
    if (code) snprintf_P(why, sizeof(why), PSTR("TLS 连接失败（%d）"), code);
    else snprintf_P(why, sizeof(why), PSTR("连不上打印机（IP 和网络是否正确？）"));
    if (gMfln == 0) gMfln = -1;  // 可能当时打印机不在线，下次重新检测分片协商
    LOG("连接拓竹打印机失败：%s", why);
    disconnect(why);
    return;
  }
  LOG("TLS 已连接（用时 %lu ms，剩余内存 %u，最大连续 %u），登录 MQTT", (unsigned long)(millis() - t0),
      (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMaxFreeBlockSize());
  char cid[24];
  snprintf(cid, sizeof(cid), "semi-ams-%06x", (unsigned)ESP.getChipId());
  uint8_t buf[96];
  gReader.reset();
  gReader.setListener(&gListener);
  gLastRx = gLastPing = millis();
  setPhase(Phase::WaitConnAck);
  send(buf, semi::mqtt::buildConnect(buf, sizeof(buf), cid, "bblp", gSettings.mqttCode, kKeepAliveS));
}

void takeActions() {
  const char* why = nullptr;
  const unsigned long sec = (unsigned long)(app::uptimeMs() / 1000);
  if (gMon.takeStart(&why)) {
    // 关掉"响应进料"的进料器只记录，不送料（多台接同一台打印机时，不用的那几台关掉）
    if (gSettings.mqttAutoLoad) {
      snprintf_P(gEvent, sizeof(gEvent), PSTR("[%lu] 开始打印 → 送料"), sec);
      app::requestPrinterStart(why);
    } else {
      snprintf_P(gEvent, sizeof(gEvent), PSTR("[%lu] 开始打印（未开启响应进料）"), sec);
      LOG(SEMI_PS "（未开启响应进料，不送料）", why);
    }
    bump();
  }
  if (gMon.takeUnload(&why)) {
    if (gSettings.mqttAutoUnload) {
      snprintf_P(gEvent, sizeof(gEvent), PSTR("[%lu] 打印机退料 → 退料"), sec);
      app::requestPrinterUnload(why);
    } else {
      snprintf_P(gEvent, sizeof(gEvent), PSTR("[%lu] 打印机退料（未开启响应退料）"), sec);
      LOG(SEMI_PS "（未开启响应退料，不退料）", why);
    }
    bump();
  }
}

}  // namespace

namespace printer {

// 记下连接用的设置；和上次记下的不一样时返回 true（需要重新连接）
static bool rememberConnSettings() {
  static bool lastEnable = false;
  static char lastIp[sizeof(gSettings.mqttIp)], lastSn[sizeof(gSettings.mqttSn)], lastCode[sizeof(gSettings.mqttCode)];
  const bool changed = lastEnable != gSettings.mqttEnable || strcmp(lastIp, gSettings.mqttIp) ||
                       strcmp(lastSn, gSettings.mqttSn) || strcmp(lastCode, gSettings.mqttCode);
  lastEnable = gSettings.mqttEnable;
  strlcpy(lastIp, gSettings.mqttIp, sizeof(lastIp));
  strlcpy(lastSn, gSettings.mqttSn, sizeof(lastSn));
  strlcpy(lastCode, gSettings.mqttCode, sizeof(lastCode));
  return changed;
}

void begin() {
  rememberConnSettings();
  gMon.setLogger(monLog);
  gMon.setUnloadWaitMs(gSettings.mqttUnloadWaitS * 1000UL);
  gReader.setListener(&gListener);
  gNextTry = millis() + 3000;
}

void applySettings() {
  gMon.setUnloadWaitMs(gSettings.mqttUnloadWaitS * 1000UL);
  // 只改了退料等待、响应进料/退料时不用重新连接（打印中改也不会打断正在等的退料）
  if (!rememberConnSettings()) {
    bump();
    return;
  }
  gMon.cancelPending();
  gNoPushall = false;
  gMfln = -1;
  gBackoff = kBackoffMinMs;
  if (gPhase != Phase::Idle) disconnect(PSTR("打印机设置已修改，重新连接"), 1000);
  else gNextTry = millis() + 1000;
  gErr[0] = 0;
  bump();
}

void setEnabled(bool on) {
  if (gSettings.mqttEnable == on) return;
  gSettings.mqttEnable = on;
  settingsSave();
  LOG("拓竹打印机 MQTT 已" SEMI_PS, on ? PSTR("开启") : PSTR("关闭"));
  applySettings();
}

bool enabled() { return gSettings.mqttEnable; }

bool connected() { return gPhase == Phase::Ready; }

uint32_t revision() { return gRev; }

void loop() {
  const uint32_t now = millis();
  const bool on = gSettings.mqttEnable && settingsMqttConfigured();
  if (on != gWasEnabled) {
    gWasEnabled = on;
    gMon.cancelPending();
    bump();
  }
  if (on) {
    gMon.tick(now);  // 退料等待超时（断线期间也继续计时）
    takeActions();
  }
  if (!on) {
    if (gPhase != Phase::Idle) disconnect(gSettings.mqttEnable ? PSTR("打印机设置不完整") : PSTR("MQTT 已关闭"), 1000);
    return;
  }
  if (Update.isRunning()) {  // 固件升级时释放内存
    if (gPhase != Phase::Idle) disconnect(PSTR("固件升级中"), kBackoffMaxMs);
    return;
  }
  if (!net::connected()) {
    if (gPhase != Phase::Idle) disconnect(PSTR("WiFi 已断开"));
    return;
  }
  // 内存快用完时先断开打印机（释放约 12KB），否则点灯、mDNS 下一次申请内存失败就会崩溃重启
  if (gPhase != Phase::Idle && ESP.getFreeHeap() < kLowHeap) {
    char why[48];
    snprintf_P(why, sizeof(why), PSTR("内存不足（空闲 %u），暂时断开"), (unsigned)ESP.getFreeHeap());
    disconnect(why, kBackoffMaxMs);
    return;
  }

  if (gPhase == Phase::Idle) {
    // TLS 握手要阻塞一两秒：只在电机停着的时候连接，避免送料时错过 FULL
    if ((int32_t)(now - gNextTry) >= 0 && app::motorStopped()) {
      // 先让点灯完成初始化，两者同时进行内存不够（见 cloud::settled）
      if (!cloud::settled()) {
        if (!gWaitCloud) {
          gWaitCloud = true;
          LOG("等点灯初始化完成后再连接打印机（同时进行内存不够）");
          bump();
        }
        return;
      }
      if (gWaitCloud) {
        gWaitCloud = false;
        bump();
      }
      connectNow();
    }
    return;
  }

  // 读取数据（每次最多处理 2KB，避免长时间占用 loop）。先读完缓冲里的数据再判断是否断开：
  // 访问码错误时打印机会发完 CONNACK 马上关闭连接，要先把 CONNACK 读出来
  uint8_t buf[256];
  size_t budget = 2048;
  while (budget && gTls->available() > 0) {
    const int n = gTls->read(buf, sizeof(buf));
    if (n <= 0) break;
    gLastRx = millis();
    gReader.feed(buf, (size_t)n);
    if (gReader.error()) {
      disconnect(PSTR("MQTT 数据格式错误"));
      return;
    }
    if (gPhase == Phase::Idle) return;  // 回调里断开了
    budget = (size_t)n >= budget ? 0 : budget - n;
  }
  takeActions();
  if (gTls->available() <= 0 && !gTls->connected()) {
    disconnect(PSTR("打印机断开了连接"));
    return;
  }

  const uint32_t t = millis();
  if ((gPhase == Phase::WaitConnAck || gPhase == Phase::WaitSubAck) && t - gPhaseSince > kReplyTimeoutMs) {
    disconnect(gPhase == Phase::WaitConnAck ? PSTR("打印机没有回应登录") : PSTR("打印机没有回应订阅"));
    return;
  }
  if (t - gLastRx > kSilenceMs) {
    disconnect(PSTR("打印机长时间没有数据"));
    return;
  }
  if (gPhase == Phase::Ready && t - gLastPing > kPingEveryMs) {
    gLastPing = t;
    uint8_t b[2];
    send(b, semi::mqtt::buildPingReq(b, sizeof(b)));
  }
}

String statusText() {
  if (!gSettings.mqttEnable) return F("未启用");
  if (!settingsMqttConfigured()) return F("未设置打印机 IP / 序列号 / 访问码");
  switch (gPhase) {
    case Phase::Ready: return F("已连接");
    case Phase::WaitConnAck:
    case Phase::WaitSubAck: return F("连接中");
    case Phase::Idle: break;
  }
  if (!net::connected()) return F("等待 WiFi");
  if (gWaitCloud) return F("等点灯初始化完成");
  const int32_t left = (int32_t)(gNextTry - millis());
  String s;
  if (left > 0) {
    s += (long)((left + 999) / 1000);
    s += F(" 秒后连接");
  } else {
    s += F("准备连接");
  }
  if (gErr[0]) {
    s += F("（");
    s += gErr;
    s += F("）");
  }
  return s;
}

String printerText() {
  if (!gMon.messages()) return F("-");
  String s = gMon.gcodeState()[0] ? gMon.gcodeState() : "?";
  const int hw = gMon.hwSwitch();
  s += F(" · 打印机里");
  s += hw < 0 ? F("耗材未知") : (hw ? F("有耗材") : F("无耗材"));
  const int ext = gMon.externalActive();
  if (ext == 1) s += F(" · 正在用外置料盘");
  else if (ext == 0) s += F(" · 没有用外置料盘");
  if (gMon.unloadPending()) s += F(" · 等待打印机退料");
  return s;
}

const char* lastEvent() { return gEvent[0] ? gEvent : "-"; }

}  // namespace printer
