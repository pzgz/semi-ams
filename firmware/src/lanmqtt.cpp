#include "lanmqtt.h"

#include <ctype.h>
#include <ESP8266WiFi.h>

#include "app.h"
#include "fsm.h"
#include "mqtt_codec.h"
#include "netinfo.h"
#include "pgm.h"
#include "settings.h"

// 局域网 MQTT 客户端：普通 TCP + MQTT 3.1.1。
// 状态用 retained 发布，服务器重启后也能拿到最后一份；availability 用遗嘱（LWT）：
// 本机掉电或断网时服务器会自动把 offline 发出去，不需要本机去发。
// 连上服务器后订阅 <前缀>control，收到的命令先核对 mac，再转成和网页按钮相同的动作。
// 全程非阻塞：连接带 2 秒超时，收数据按可用字节读，重连走指数退避，不影响 10ms 的 tick。

namespace {

const uint16_t kKeepAliveS = 60;
const uint32_t kPingEveryMs = 30000;     // 保活：每 30 秒一次 PINGREQ
const uint32_t kSilenceMs = 90000;       // 这么久没收到任何数据就认为连接已断
const uint32_t kConnAckTimeoutMs = 5000;
const uint32_t kBackoffMinMs = 5000;
const uint32_t kBackoffMaxMs = 60000;
const uint32_t kMinGapMs = 500;          // 状态连续变化时的最小发布间隔，避免刷爆服务器
const uint32_t kIdlePublishMs = 30000;   // 状态没变化也定期发一次，方便服务器判断数据是新鲜的

enum class Phase : uint8_t { Idle, WaitConnAck, Ready };

WiFiClient gClient;
semi::mqtt::Reader gReader;
Phase gPhase = Phase::Idle;
uint32_t gPhaseSince = 0;
uint32_t gNextTry = 0;
uint32_t gBackoff = kBackoffMinMs;
uint32_t gLastRx = 0, gLastPing = 0, gLastPub = 0;
uint32_t gPubCount = 0;
uint32_t gLastRev = 0;
char gErr[64] = "";  // 失败原因（写入时已从 Flash 拷到内存）

void setPhase(Phase p) {
  gPhase = p;
  gPhaseSince = millis();
}

bool sendRaw(const uint8_t* buf, size_t n) {
  if (!n || !gClient.connected()) return false;
  return gClient.write(buf, n) == n;
}

void disconnect(const char* why, uint32_t retryInMs = 0);

// 发布一条 QoS 0 消息。retain 位是固定头的最低位（0x01），buildPublish 不带这个参数，发之前补上
bool publish(const char* suffix, const char* payload, bool retain) {
  char topic[48];
  snprintf(topic, sizeof(topic), "%s%s", gSettings.lanMqttTopic, suffix);
  uint8_t buf[288];
  size_t n = semi::mqtt::buildPublish(buf, sizeof(buf), topic, payload, strlen(payload));
  if (!n) return false;
  if (retain) buf[0] |= 0x01;
  return sendRaw(buf, n);
}

void publishState(bool force) {
  const app::Status s = app::status();
  const uint32_t now = millis();
  if (!force) {
    if (now - gLastPub < kMinGapMs) return;
    if (s.rev == gLastRev && now - gLastPub < kIdlePublishMs) return;  // 没变化，等心跳
  }
  char state[24], motor[16];
  semi_copy_P(state, semi::Fsm::stateName(s.state), sizeof(state));
  semi_copy_P(motor, semi::Fsm::motorName(s.motor), sizeof(motor));

  // 设备身份：没联网或取不到就发空串，订阅端据此判断数据是否可信
  char ipStr[16], macStr[18];
  netinfo::ip(ipStr, sizeof(ipStr));
  netinfo::mac(macStr, sizeof(macStr));

  char json[288];
  snprintf(json, sizeof(json),
           "{\"state\":\"%s\",\"motor\":\"%s\",\"duty\":%u,\"in\":%d,\"ready\":%d,\"lack\":%d,\"full\":%d,"
           "\"rssi\":%d,\"heap\":%u,\"uptime\":%lu,\"ip\":\"%s\",\"mac\":\"%s\"}",
           state, motor, (unsigned)s.duty, s.in ? 1 : 0, s.ready ? 1 : 0, s.lack ? 1 : 0, s.full ? 1 : 0,
           WiFi.RSSI(), (unsigned)ESP.getFreeHeap(), (unsigned long)(app::uptimeMs() / 1000), ipStr, macStr);
  if (publish("state", json, true)) {
    gLastPub = now;
    gLastRev = s.rev;
    gPubCount++;
  } else {
    disconnect(PSTR("发布状态失败"));
  }
}

// ---------- 控制命令（订阅 <前缀>control） ----------
const uint16_t kSubPacketId = 1;  // 每次连接只订阅一次，包 ID 固定即可（clean session）
const size_t kMsgMax = 256;       // 控制消息远小于这个长度，超出的直接丢弃

char gMsgBuf[kMsgMax + 1];
size_t gMsgLen = 0;
bool gMsgOverflow = false;
bool gMsgIsControl = false;

bool topicIsControl(const char* t) {
  char want[48];
  snprintf(want, sizeof(want), "%scontrol", gSettings.lanMqttTopic);
  return strcmp(t, want) == 0;
}

// 从简单 JSON 里取字符串字段：找 "key"，跳过空白和冒号，把引号里的内容拷出来。
// 控制消息的字段都是字符串（ip / mac / on / off），不需要完整的 JSON 解析器
bool jsonStr(const char* json, const char* key, char* out, size_t cap) {
  char pat[20];
  if (cap < 2) return false;
  if (snprintf(pat, sizeof(pat), "\"%s\"", key) >= (int)sizeof(pat)) return false;
  const char* p = strstr(json, pat);
  if (!p) return false;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != ':') return false;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != '"') return false;
  size_t i = 0;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) p++;  // 反转义，本用途基本不会出现
    if (i + 1 < cap) out[i++] = *p;
    p++;
  }
  out[i] = 0;
  return i > 0;
}

// mac 比较不区分大小写：手写消息时常见大写
bool sameMac(const char* a, const char* b) {
  if (!*a || !*b) return false;
  for (; *a && *b; a++, b++)
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
  return *a == 0 && *b == 0;
}

bool valueIsOn(const char* v) { return !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "true"); }
bool valueIsOff(const char* v) { return !strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "false"); }

// 和网页按钮不同，这里按文档的 on / off 语义做成幂等：
// on = 开始打印（已经在打印或退料就不动），off = 结束打印（只有打印中才结束）。
// 消息重投（QoS 1）或重复下发都不会让设备来回切换
void doStartStop(bool on) {
  const semi::State st = app::status().state;
  if (on) {
    if (st == semi::State::Printing || st == semi::State::Unloading) {
      LOG("局域网 MQTT：已经在打印，忽略开始");
      return;
    }
  } else if (st != semi::State::Printing) {
    LOG("局域网 MQTT：当前不是打印中，忽略结束");
    return;
  }
  app::requestStartStop(PSTR("局域网 MQTT"));
}

void doEmergency(bool on) {
  const bool now = app::status().state == semi::State::Emergency;
  if (on == now) {
    LOG("局域网 MQTT：紧急停止状态没变，忽略");
    return;
  }
  app::requestEmergency(PSTR("局域网 MQTT"));
}

void handleControl(const char* json) {
  char want[18];
  if (!jsonStr(json, "mac", want, sizeof(want))) {
    LOG("局域网 MQTT：命令里没有 mac，已忽略");
    return;
  }
  char self[18];
  netinfo::mac(self, sizeof(self));
  if (!sameMac(want, self)) {  // self 为空（取不到 mac）时也在这里被挡掉
    LOG("局域网 MQTT：mac 不是本机（%s），已忽略", want);
    return;
  }

  char v[8];
  if (jsonStr(json, "start", v, sizeof(v))) {
    if (valueIsOn(v)) {
      LOG("局域网 MQTT：开始打印");
      doStartStop(true);
    } else if (valueIsOff(v)) {
      LOG("局域网 MQTT：结束打印");
      doStartStop(false);
    } else {
      LOG("局域网 MQTT：start 的值不是 on/off，已忽略");
    }
  } else if (jsonStr(json, "emg", v, sizeof(v))) {
    if (valueIsOn(v)) {
      LOG("局域网 MQTT：紧急停止");
      doEmergency(true);
    } else if (valueIsOff(v)) {
      LOG("局域网 MQTT：解除紧急停止");
      doEmergency(false);
    } else {
      LOG("局域网 MQTT：emg 的值不是 on/off，已忽略");
    }
  } else if (jsonStr(json, "jogf", v, sizeof(v))) {
    if (valueIsOn(v)) {
      LOG("局域网 MQTT：正转 2 秒");
      if (!app::requestJog(semi::MotorDir::Forward, gSettings.p.feedSpeed, 2000))
        LOG("局域网 MQTT：点动被拒绝（只在 无耗材/耗材就绪/已退料 状态下可用）");
    }
  } else if (jsonStr(json, "jogr", v, sizeof(v))) {
    if (valueIsOn(v)) {
      LOG("局域网 MQTT：反转 2 秒");
      if (!app::requestJog(semi::MotorDir::Reverse, gSettings.p.feedSpeed, 2000))
        LOG("局域网 MQTT：点动被拒绝（只在 无耗材/耗材就绪/已退料 状态下可用）");
    }
  } else if (jsonStr(json, "restart", v, sizeof(v))) {
    if (valueIsOn(v)) {
      LOG("局域网 MQTT：重启");
      app::requestRestart(PSTR("局域网 MQTT"));
    }
  } else {
    LOG("局域网 MQTT：命令里没有可识别的动作（start/emg/jogf/jogr/restart）");
  }
}

// 订阅控制主题。QoS 1：局域网偶发丢包时命令不至于丢掉，收到后要回 PUBACK（见 onPublishEnd）
bool subscribeControl() {
  char topic[48];
  snprintf(topic, sizeof(topic), "%scontrol", gSettings.lanMqttTopic);
  uint8_t buf[96];
  const size_t n = semi::mqtt::buildSubscribe(buf, sizeof(buf), kSubPacketId, topic, 1);
  return n && sendRaw(buf, n);
}

class Listener : public semi::mqtt::Listener {
 public:
  void onConnAck(uint8_t rc) override {
    if (rc != 0) {
      const char* why;
      switch (rc) {
        case 1: why = PSTR("服务器不支持 MQTT 3.1.1"); break;
        case 2: why = PSTR("客户端 ID 被拒绝"); break;
        case 3: why = PSTR("服务器不可用"); break;
        case 4: why = PSTR("用户名或密码错误"); break;
        case 5: why = PSTR("未授权（服务器要求认证）"); break;
        default: why = PSTR("服务器拒绝连接"); break;
      }
      disconnect(why);
      return;
    }
    gBackoff = kBackoffMinMs;
    gErr[0] = 0;
    setPhase(Phase::Ready);
    publish("availability", "online", true);
    LOG("已连接局域网 MQTT %s:%u", gSettings.lanMqttIp, (unsigned)gSettings.lanMqttPort);
    if (!subscribeControl()) {  // 连上以后才订阅控制主题
      disconnect(PSTR("订阅控制主题失败"));
      return;
    }
    publishState(true);  // 连上先发一份完整状态
  }

  void onPingResp() override {}

  void onSubAck(uint16_t, uint8_t result) override {
    if (result == 0x80) LOG("局域网 MQTT：服务器拒绝订阅控制主题（检查 ACL）");
    else LOG("局域网 MQTT：已订阅 %scontrol", gSettings.lanMqttTopic);
  }

  void onPublishBegin(const char* topic, size_t payloadLen) override {
    gMsgIsControl = topicIsControl(topic);
    gMsgLen = 0;
    gMsgOverflow = payloadLen >= kMsgMax;
  }

  void onPublishData(const uint8_t* data, size_t n) override {
    if (!gMsgIsControl || gMsgOverflow) return;
    if (gMsgLen + n >= kMsgMax) {
      gMsgOverflow = true;
      return;
    }
    memcpy(gMsgBuf + gMsgLen, data, n);
    gMsgLen += n;
  }

  void onPublishEnd(uint8_t qos, uint16_t packetId) override {
    if (qos == 1) {  // 订阅时用了 QoS 1，不回 PUBACK 服务器会一直重投
      uint8_t b[4];
      sendRaw(b, semi::mqtt::buildPubAck(b, sizeof(b), packetId));
    }
    if (gMsgIsControl && !gMsgOverflow && gMsgLen) {
      gMsgBuf[gMsgLen] = 0;
      handleControl(gMsgBuf);
    } else if (gMsgIsControl && gMsgOverflow) {
      LOG("局域网 MQTT：控制消息太长，已忽略");
    }
    gMsgIsControl = false;
    gMsgLen = 0;
    gMsgOverflow = false;
  }
};
Listener gListener;

void disconnect(const char* why, uint32_t retryInMs) {
  gReader.stop();  // 可能是在解析回调里断开的：剩下的数据不再处理
  if (gPhase == Phase::Ready && gClient.connected()) {
    publish("availability", "offline", true);  // 主动断开时自己发，遗嘱只在异常掉线时才用得上
    uint8_t b[2];
    gClient.write(b, semi::mqtt::buildDisconnect(b, sizeof(b)));
  }
  if (gPhase != Phase::Idle) {
    LOG("局域网 MQTT 断开：" SEMI_PS, why);
    semi_copy_P(gErr, why, sizeof(gErr));
  }
  gClient.stop();
  if (!retryInMs) {
    retryInMs = gBackoff;
    gBackoff = gBackoff * 2 > kBackoffMaxMs ? kBackoffMaxMs : gBackoff * 2;
  }
  gNextTry = millis() + retryInMs;
  setPhase(Phase::Idle);
}

void connectNow() {
  IPAddress ip;
  if (!ip.fromString(gSettings.lanMqttIp)) {
    disconnect(PSTR("服务器 IP 格式不对"), kBackoffMaxMs);
    return;
  }
  gClient.setTimeout(2000);
  if (!gClient.connect(ip, gSettings.lanMqttPort)) {
    disconnect(PSTR("连不上 MQTT 服务器（IP、端口是否正确？服务是否启动？）"));
    return;
  }
  char clientId[32];
  snprintf(clientId, sizeof(clientId), "semi-ams-%06x", (unsigned)(ESP.getChipId() & 0xFFFFFF));
  char willTopic[48];
  snprintf(willTopic, sizeof(willTopic), "%savailability", gSettings.lanMqttTopic);
  uint8_t buf[160];
  const size_t n = semi::mqtt::buildConnectWill(buf, sizeof(buf), clientId, nullptr, nullptr, kKeepAliveS,
                                                willTopic, "offline", true);
  if (!n || !sendRaw(buf, n)) {
    disconnect(PSTR("发送连接请求失败"));
    return;
  }
  gReader.setListener(&gListener);  // 不设的话 CONNACK 解析完没人接手，会一直等到超时
  gReader.reset();
  gLastRx = millis();
  setPhase(Phase::WaitConnAck);
}

void pump() {
  if (!gClient.connected()) {
    disconnect(PSTR("连接被服务器关闭"));
    return;
  }
  while (gClient.available()) {
    uint8_t b[128];
    const size_t n = gClient.read(b, sizeof(b));
    if (!n) break;
    gLastRx = millis();
    gReader.feed(b, n);
    if (gReader.error()) {
      disconnect(PSTR("服务器返回的数据格式不对"));
      return;
    }
  }
}

}  // namespace

namespace lanmqtt {

void loop() {
  if (!gSettings.lanMqttEnable || !gSettings.lanMqttIp[0]) {
    if (gPhase != Phase::Idle) disconnect(PSTR("功能未启用"), 1);
    gErr[0] = 0;
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {  // WiFi 没连上时不要空转重试
    if (gPhase != Phase::Idle) disconnect(PSTR("WiFi 未连接"), 1);
    return;
  }

  switch (gPhase) {
    case Phase::Idle:
      if ((int32_t)(millis() - gNextTry) >= 0) connectNow();
      break;

    case Phase::WaitConnAck:
      pump();
      if (gPhase == Phase::WaitConnAck && millis() - gPhaseSince > kConnAckTimeoutMs)
        disconnect(PSTR("服务器没有回应连接请求"));
      break;

    case Phase::Ready:
      pump();
      if (gPhase != Phase::Ready) break;  // pump 里可能已经断开
      if (millis() - gLastRx > kSilenceMs) {
        disconnect(PSTR("服务器长时间没有响应"));
        break;
      }
      if (millis() - gLastPing > kPingEveryMs) {
        gLastPing = millis();
        uint8_t b[2];
        sendRaw(b, semi::mqtt::buildPingReq(b, sizeof(b)));
      }
      publishState(false);
      break;
  }
}

void applySettings() {
  if (gPhase != Phase::Idle) disconnect(PSTR("设置已修改"), 1);
  gBackoff = kBackoffMinMs;
  gNextTry = 0;  // 下一轮 loop 立刻按新设置连
  gErr[0] = 0;
  gLastPub = 0;
  gLastRev = 0;
}

bool enabled() { return gSettings.lanMqttEnable && gSettings.lanMqttIp[0]; }

bool connected() { return gPhase == Phase::Ready; }

uint32_t publishCount() { return gPubCount; }

const char* lastError() { return gErr; }

String statusText() {
  if (!gSettings.lanMqttEnable) return F("未启用");
  if (!gSettings.lanMqttIp[0]) return F("未填服务器 IP");
  switch (gPhase) {
    case Phase::Ready:
      return String(gSettings.lanMqttIp) + ":" + String(gSettings.lanMqttPort);
    case Phase::WaitConnAck:
      return F("连接中");
    default:
      break;
  }
  const long left = (long)(gNextTry - millis()) / 1000;
  String s;
  if (left > 0) s = String(left) + F(" 秒后重连");
  else s = F("正在重连");
  if (gErr[0]) {
    s += F("（");
    s += gErr;
    s += F("）");
  }
  return s;
}

}  // namespace lanmqtt
