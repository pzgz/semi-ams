#include "lanmqtt.h"

#include <ESP8266WiFi.h>
#include <Updater.h>

#include "app.h"
#include "cloud.h"
#include "fsm.h"
#include "lanmqtt_proto.h"
#include "mqtt_codec.h"
#include "netinfo.h"
#include "pgm.h"
#include "settings.h"

// 局域网 MQTT 客户端：普通 TCP + MQTT 3.1.1，topic 见 lanmqtt_proto.h。
// 状态用 retain 发布：服务器重启、群控程序后来才订阅，也能拿到每台的最后一份。在线情况用遗嘱（LWT）：
// 本机掉电或断网时，服务器自动把 offline 发出去。
// 开了"允许远程控制"才订阅 <前缀>control。收到的命令先核对 mac，再转成和网页按钮相同的动作；
// 服务器补发的保留消息一律不执行，否则每次重连都会重放一遍（restart 会无限重启）。
// connect() 会阻塞，连不上服务器时最长 2 秒（和连接打印机一样），所以只在电机停着的时候连；
// 连上以后收发都不阻塞，断开后按指数退避重连。

namespace {

const uint16_t kKeepAliveS = 60;
const uint32_t kPingEveryMs = 30000;      // 保活：每 30 秒一次 PINGREQ
const uint32_t kSilenceMs = 90000;        // 这么久没收到任何数据就认为连接已断
const uint32_t kConnAckTimeoutMs = 5000;
const uint32_t kBackoffMinMs = 5000;
const uint32_t kBackoffMaxMs = 60000;
const uint32_t kMinGapMs = 500;           // 状态连续变化时的最小发布间隔，避免刷爆服务器
const uint32_t kIdlePublishMs = 30000;    // 状态没变化也定期发一次，方便订阅端判断数据是新鲜的
const uint32_t kMinHeapToConnect = 8000;  // 连接要 1–2KB（TCP 连接和收发缓冲），给点灯和打印机留出余量
const uint32_t kLowHeap = 3000;           // 连接中空闲内存低于这个值就先断开（和打印机一样）
const size_t kTopicMax = 64;              // 前缀最长 32 + 设备 ID 12 + "/availability" 13 + 结尾 0
const size_t kMsgMax = 160;               // 控制消息一般不到 100 字节，超出的直接丢弃

enum class Phase : uint8_t { Idle, WaitConnAck, Ready };
enum class Wait : uint8_t { None, Cloud, Heap };  // Idle 时为什么还没连

WiFiClient gClient;
semi::mqtt::Reader gReader;
Phase gPhase = Phase::Idle;
Wait gWait = Wait::None;
uint32_t gPhaseSince = 0;
uint32_t gNextTry = 0;
uint32_t gBackoff = kBackoffMinMs;
uint32_t gLastRx = 0, gLastPing = 0, gLastPub = 0;
uint32_t gPubCount = 0;
uint32_t gLastRev = 0;
char gId[13] = "";   // 设备 ID（MAC 去掉冒号），连接时生成
char gErr[64] = "";  // 失败原因（写入时已从 Flash 拷到内存）
bool gRetainedWarned = false;

void setPhase(Phase p) {
  gPhase = p;
  gPhaseSince = millis();
}

// <前缀><设备ID>/<suffix>，suffix 是 Flash 字符串
void deviceTopic(char* out, size_t cap, const char* suffixP) {
  snprintf_P(out, cap, PSTR("%s%s/%S"), gSettings.lanMqttTopic, gId, suffixP);
}

void controlTopic(char* out, size_t cap) { snprintf_P(out, cap, PSTR("%scontrol"), gSettings.lanMqttTopic); }

// availability 的内容：{"state":"online","ip":"…","mac":"…"}（取不到 ip、mac 时为空串）
void availabilityJson(char* out, size_t cap, bool online) {
  char ip[16], mac[18];
  netinfo::ip(ip, sizeof(ip));
  netinfo::mac(mac, sizeof(mac));
  snprintf_P(out, cap, PSTR("{\"state\":\"%S\",\"ip\":\"%s\",\"mac\":\"%s\"}"), online ? PSTR("online") : PSTR("offline"),
             ip, mac);
}

bool sendRaw(const uint8_t* buf, size_t n) {
  if (!n || !gClient.connected()) return false;
  return gClient.write(buf, n) == n;
}

void disconnect(const char* why, uint32_t retryInMs = 0);

// 发布一条 QoS 0 消息。retain 位是固定头的最低位（0x01），buildPublish 不带这个参数，发之前补上。
// 返回 false = 发送失败（连接有问题）；消息太长装不下只记日志，不算连接问题
bool publish(const char* topic, const char* payload, bool retain) {
  uint8_t buf[352];
  const size_t n = semi::mqtt::buildPublish(buf, sizeof(buf), topic, payload, strlen(payload));
  if (!n) {
    LOG("局域网 MQTT：消息太长，没有发出（%s）", topic);
    return true;
  }
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
  char ip[16], mac[18];
  netinfo::ip(ip, sizeof(ip));
  netinfo::mac(mac, sizeof(mac));

  // state 是中文名（给人看），key 是固定的英文（给自动化判断用，见 Fsm::stateKey）
  char json[300];
  snprintf_P(json, sizeof(json),
             PSTR("{\"state\":\"%s\",\"key\":\"%s\",\"motor\":\"%s\",\"duty\":%u,\"in\":%d,\"ready\":%d,\"lack\":%d,"
                  "\"full\":%d,\"rssi\":%d,\"heap\":%u,\"uptime\":%lu,\"ip\":\"%s\",\"mac\":\"%s\"}"),
             state, semi::Fsm::stateKey(s.state), motor, (unsigned)s.duty, s.in ? 1 : 0, s.ready ? 1 : 0,
             s.lack ? 1 : 0, s.full ? 1 : 0, WiFi.RSSI(), (unsigned)ESP.getFreeHeap(),
             (unsigned long)(app::uptimeMs() / 1000), ip, mac);
  char topic[kTopicMax];
  deviceTopic(topic, sizeof(topic), PSTR("state"));
  if (publish(topic, json, true)) {
    gLastPub = now;
    gLastRev = s.rev;
    gPubCount++;
  } else {
    disconnect(PSTR("发布状态失败"));
  }
}

// ---------- 控制命令（订阅 <前缀>control） ----------
const uint16_t kSubPacketId = 1;  // 每次连接只订阅一次，包 ID 固定即可（clean session）

char gMsgBuf[kMsgMax + 1];
size_t gMsgLen = 0;
bool gMsgOverflow = false;
bool gMsgIsControl = false;
bool gMsgRetained = false;

bool topicIsControl(const char* t) {
  char want[kTopicMax];
  controlTopic(want, sizeof(want));
  return strcmp(t, want) == 0;
}

// 和网页按钮不同，这里按 on / off 语义做成幂等：
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

void doJog(semi::MotorDir dir) {
  if (!app::requestJog(dir, gSettings.p.feedSpeed, 2000))
    LOG("局域网 MQTT：点动被拒绝（只在 无耗材/耗材就绪/已退料 状态下可用）");
}

void handleControl(const char* json, size_t n) {
  typedef semi::lan::Action A;
  semi::lan::Command c;
  if (!semi::lan::parseControl(json, n, c)) {
    LOG("局域网 MQTT：控制消息不是有效的 JSON 对象，已忽略");
    return;
  }
  if (!c.mac[0]) {
    LOG("局域网 MQTT：控制消息里没有 mac，已忽略");
    return;
  }
  char self[18];
  netinfo::mac(self, sizeof(self));
  if (!semi::lan::sameMac(c.mac, self)) return;  // 发给别的设备的（几台共用 control），不记日志
  if (c.action == A::None) {
    LOG("局域网 MQTT：控制消息里没有可识别的动作（start/emg/jogf/jogr/restart）");
    return;
  }
  if (c.on < 0) {
    LOG("局域网 MQTT：动作的值不是 on/off、true/false 或 1/0，已忽略");
    return;
  }
  const bool on = c.on == 1;
  switch (c.action) {
    case A::Start:
      LOG("局域网 MQTT：" SEMI_PS, on ? PSTR("开始打印") : PSTR("结束打印"));
      doStartStop(on);
      break;
    case A::Emergency:
      LOG("局域网 MQTT：" SEMI_PS, on ? PSTR("紧急停止") : PSTR("解除紧急停止"));
      doEmergency(on);
      break;
    case A::JogForward:
      if (!on) break;
      LOG("局域网 MQTT：正转 2 秒");
      doJog(semi::MotorDir::Forward);
      break;
    case A::JogReverse:
      if (!on) break;
      LOG("局域网 MQTT：反转 2 秒");
      doJog(semi::MotorDir::Reverse);
      break;
    case A::Restart:
      if (!on) break;
      LOG("局域网 MQTT：重启");
      app::requestRestart(PSTR("局域网 MQTT"));
      break;
    case A::None:
      break;
  }
}

// 订阅控制主题。QoS 1：局域网偶发丢包时命令不至于丢掉，收到后要回 PUBACK（见 onPublishEnd）
bool subscribeControl() {
  char topic[kTopicMax];
  controlTopic(topic, sizeof(topic));
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
        case 5: why = PSTR("未授权（服务器要求登录，填用户名和密码）"); break;
        default: why = PSTR("服务器拒绝连接"); break;
      }
      disconnect(why, (rc == 4 || rc == 5) ? kBackoffMaxMs : 0);
      return;
    }
    gBackoff = kBackoffMinMs;
    gErr[0] = 0;
    setPhase(Phase::Ready);
    LOG("已连接局域网 MQTT %s:%u，本机 topic：%s%s/…" SEMI_PS, gSettings.lanMqttIp, (unsigned)gSettings.lanMqttPort,
        gSettings.lanMqttTopic, gId, gSettings.lanMqttControl ? PSTR("，接受远程控制") : PSTR(""));
    char topic[kTopicMax], msg[96];
    deviceTopic(topic, sizeof(topic), PSTR("availability"));
    availabilityJson(msg, sizeof(msg), true);
    if (!publish(topic, msg, true)) {
      disconnect(PSTR("发布在线状态失败"));
      return;
    }
    if (gSettings.lanMqttControl && !subscribeControl()) {
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
    gMsgIsControl = gSettings.lanMqttControl && topicIsControl(topic);
    gMsgRetained = gReader.retained();
    gMsgLen = 0;
    gMsgOverflow = payloadLen > kMsgMax;
  }

  void onPublishData(const uint8_t* data, size_t n) override {
    if (!gMsgIsControl || gMsgOverflow || gMsgRetained) return;
    if (gMsgLen + n > kMsgMax) {
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
    if (gMsgIsControl) {
      if (gMsgRetained) {
        if (!gRetainedWarned) {
          gRetainedWarned = true;
          LOG("局域网 MQTT：服务器上有一条保留的控制消息，不执行（发控制命令时不要勾选 retain）");
        }
      } else if (gMsgOverflow) {
        LOG("局域网 MQTT：控制消息太长，已忽略");
      } else if (gMsgLen) {
        gMsgBuf[gMsgLen] = 0;
        handleControl(gMsgBuf, gMsgLen);
      }
    }
    gMsgIsControl = false;
    gMsgRetained = false;
    gMsgLen = 0;
    gMsgOverflow = false;
  }
};
Listener gListener;

void disconnect(const char* why, uint32_t retryInMs) {
  gReader.stop();  // 可能是在解析回调里断开的：剩下的数据不再处理
  if (gPhase == Phase::Ready && gClient.connected()) {
    // 主动断开时自己发 offline；遗嘱只在异常掉线时由服务器代发
    char topic[kTopicMax], msg[96];
    deviceTopic(topic, sizeof(topic), PSTR("availability"));
    availabilityJson(msg, sizeof(msg), false);
    publish(topic, msg, true);
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
  char mac[18];
  netinfo::mac(mac, sizeof(mac));
  semi::lan::deviceId(mac, gId, sizeof(gId));
  if (!gId[0]) {
    disconnect(PSTR("取不到本机 MAC"), kBackoffMaxMs);
    return;
  }
  gClient.setTimeout(2000);
  if (!gClient.connect(ip, gSettings.lanMqttPort)) {
    disconnect(PSTR("连不上 MQTT 服务器（IP、端口是否正确？服务是否启动？）"));
    return;
  }
  char clientId[24];
  snprintf_P(clientId, sizeof(clientId), PSTR("semi-ams-%s"), gId);
  char willTopic[kTopicMax], will[96];
  deviceTopic(willTopic, sizeof(willTopic), PSTR("availability"));
  availabilityJson(will, sizeof(will), false);
  // MQTT 3.1.1 不允许只有密码没有用户名
  const char* user = gSettings.lanMqttUser[0] ? gSettings.lanMqttUser : nullptr;
  const char* pass = user && gSettings.lanMqttPass[0] ? gSettings.lanMqttPass : nullptr;
  uint8_t buf[320];
  const size_t n =
      semi::mqtt::buildConnectWill(buf, sizeof(buf), clientId, user, pass, kKeepAliveS, willTopic, will, true);
  if (!n || !sendRaw(buf, n)) {
    disconnect(PSTR("发送连接请求失败"));
    return;
  }
  gReader.setListener(&gListener);  // 不设的话 CONNACK 解析完没人接手，会一直等到超时
  gReader.reset();
  gRetainedWarned = false;
  gLastRx = millis();
  gLastPing = millis();
  setPhase(Phase::WaitConnAck);
}

void pump() {
  // 先读完缓冲里的数据再判断是否断开：服务器拒绝登录时会发完 CONNACK 马上关闭连接
  size_t budget = 1024;  // 每次 loop 最多处理 1KB
  while (budget && gClient.available() > 0) {
    uint8_t b[128];
    const size_t n = gClient.read(b, budget < sizeof(b) ? budget : sizeof(b));
    if (!n) break;
    budget -= n;
    gLastRx = millis();
    gReader.feed(b, n);
    if (gPhase == Phase::Idle) return;  // 回调里已经断开
    if (gReader.error()) {
      disconnect(PSTR("服务器返回的数据格式不对"));
      return;
    }
  }
  if (!gClient.connected() && !gClient.available()) disconnect(PSTR("连接被服务器关闭"));
}

}  // namespace

namespace lanmqtt {

void loop() {
  if (!gSettings.lanMqttEnable || !gSettings.lanMqttIp[0]) {
    if (gPhase != Phase::Idle) disconnect(PSTR("功能未启用"), 1);
    gErr[0] = 0;
    gWait = Wait::None;
    return;
  }
  if (Update.isRunning()) {  // 固件升级时释放内存
    if (gPhase != Phase::Idle) disconnect(PSTR("固件升级中"), kBackoffMaxMs);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {  // WiFi 没连上时不要空转重试
    if (gPhase != Phase::Idle) disconnect(PSTR("WiFi 未连接"), 1);
    return;
  }
  // 内存快用完时先断开，否则点灯、mDNS 下一次申请内存失败就会崩溃重启
  if (gPhase != Phase::Idle && ESP.getFreeHeap() < kLowHeap) {
    char why[48];
    snprintf_P(why, sizeof(why), PSTR("内存不足（空闲 %u），暂时断开"), (unsigned)ESP.getFreeHeap());
    disconnect(why, kBackoffMaxMs);
    return;
  }

  switch (gPhase) {
    case Phase::Idle: {
      // connect() 连不上时会阻塞最长 2 秒：只在电机停着的时候连，避免送料时错过 FULL
      if ((int32_t)(millis() - gNextTry) < 0 || !app::motorStopped()) break;
      // 先让点灯完成初始化，同时进行内存不够（见 cloud::settled）
      if (!cloud::settled()) {
        if (gWait != Wait::Cloud) {
          gWait = Wait::Cloud;
          LOG("等点灯初始化完成后再连接局域网 MQTT（同时进行内存不够）");
        }
        break;
      }
      const uint32_t heap = ESP.getFreeHeap();
      if (heap < kMinHeapToConnect) {
        if (gWait != Wait::Heap) {
          gWait = Wait::Heap;
          LOG("空闲内存不足（%u，至少要 %u），暂不连接局域网 MQTT", (unsigned)heap, (unsigned)kMinHeapToConnect);
        }
        gNextTry = millis() + kBackoffMaxMs;
        break;
      }
      gWait = Wait::None;
      connectNow();
      break;
    }

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
        if (!sendRaw(b, semi::mqtt::buildPingReq(b, sizeof(b)))) {
          disconnect(PSTR("发送保活失败"));
          break;
        }
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
  gWait = Wait::None;
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
      return String(F("已连接 ")) + gSettings.lanMqttIp + ":" + String(gSettings.lanMqttPort);
    case Phase::WaitConnAck:
      return F("连接中");
    default:
      break;
  }
  if (gWait == Wait::Cloud) return F("等点灯初始化完成");
  String s;
  if (gWait == Wait::Heap) s = F("空闲内存不足，稍后再连");
  else {
    const long left = (long)(gNextTry - millis()) / 1000;
    if (left > 0) s = String(left) + F(" 秒后重连");
    else s = F("正在重连");
  }
  if (gErr[0]) {
    s += F("（");
    s += gErr;
    s += F("）");
  }
  return s;
}

String topicText() {
  char mac[18], id[13];
  netinfo::mac(mac, sizeof(mac));
  semi::lan::deviceId(mac, id, sizeof(id));
  String s = gSettings.lanMqttTopic;
  s += id[0] ? id : "<设备ID>";
  s += F("/state");
  return s;
}

}  // namespace lanmqtt
