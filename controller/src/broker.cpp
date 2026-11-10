#include "broker.h"

#include <stdlib.h>
#include <string.h>

#include <new>  // std::nothrow

#include "devices.h"
#include "settings.h"
#include "topic.h"

namespace ctrl {

Broker gBroker;

namespace {

enum : uint8_t {
  T_CONNECT = 1,
  T_CONNACK = 2,
  T_PUBLISH = 3,
  T_PUBACK = 4,
  T_PUBREC = 5,
  T_PUBREL = 6,
  T_PUBCOMP = 7,
  T_SUBSCRIBE = 8,
  T_SUBACK = 9,
  T_UNSUBSCRIBE = 10,
  T_UNSUBACK = 11,
  T_PINGREQ = 12,
  T_PINGRESP = 13,
  T_DISCONNECT = 14,
  T_AUTH = 15,
};

// WiFiServer::begin() 是 void，但对象自己有 operator bool()（返回内部的 _listening），
// 用它判断到底有没有真的 listen 上——端口被别的程序占住时 bind 会失败，
// 原来不看这个就会在网页上照样显示「已启动」。

size_t putLen(uint8_t* p, size_t len) {
  size_t n = 0;
  do {
    uint8_t b = len % 128;
    len /= 128;
    if (len) b |= 0x80;
    p[n++] = b;
  } while (len && n < 4);
  return n;
}

// 解析用的小游标（指向报文 body）
struct Cur {
  Cur(const uint8_t* pp, size_t nn) : p(pp), n(nn) {}
  const uint8_t* p;
  size_t n;
  size_t i = 0;
  bool u8(uint8_t* v) {
    if (i + 1 > n) return false;
    *v = p[i++];
    return true;
  }
  bool u16(uint16_t* v) {
    if (i + 2 > n) return false;
    *v = (uint16_t)(p[i] << 8 | p[i + 1]);
    i += 2;
    return true;
  }
  bool str(const char** s, size_t* len) {
    if (i + 2 > n) return false;
    size_t l = (size_t)(p[i] << 8 | p[i + 1]);
    i += 2;
    if (i + l > n) return false;
    *s = (const char*)(p + i);
    *len = l;
    i += l;
    return true;
  }
  // MQTT 5 的属性块：先一个变长长度，再是一串属性；我们不用，直接整块跳过
  bool skipProps() {
    uint32_t len = 0, mul = 1;
    for (int k = 0; k < 4; ++k) {
      if (i >= n) return false;
      const uint8_t b = p[i++];
      len += (uint32_t)(b & 0x7F) * mul;
      mul *= 128;
      if (!(b & 0x80)) {
        if (i + len > n) return false;
        i += len;
        return true;
      }
    }
    return false;
  }
};

void copyN(char* dst, size_t cap, const char* src, size_t len) {
  if (len > cap - 1) len = cap - 1;
  memcpy(dst, src, len);
  dst[len] = 0;
}

bool endsWith(const char* s, const char* suffix) {
  const size_t ls = strlen(s), lf = strlen(suffix);
  return ls >= lf && strcmp(s + ls - lf, suffix) == 0;
}

// 报文里的 payload 不是以 0 结尾，设备表的 JSON 解析要 C 字符串
char gPayload[MQTT_BODY_MAX + 1];

bool constEq(const char* a, size_t al, const char* b) {
  const size_t bl = strlen(b);
  return al == bl && memcmp(a, b, al) == 0;
}

}  // namespace

// ---------------- 启停 ----------------

void Broker::begin() {
  stop();
  port_ = gSettings.brokerPort ? gSettings.brokerPort : MQTT_PORT_DEFAULT;
  lastError_[0] = 0;
  server_.begin(port_);
  if (!server_) {  // 没进 listen 状态：端口被占（比如另一个程序已经用了这个端口）
    snprintf(lastError_, sizeof(lastError_), "端口 %u 起不来，可能被占用了", port_);
    running_ = false;
    server_.end();
    Serial.printf("[MQTT] 启动失败：%s\n", lastError_);
    return;
  }
  server_.setNoDelay(true);
  running_ = true;
  Serial.printf("[MQTT] 服务端监听 0.0.0.0:%u，不设客户端数上限（socket 预算 %d 个），%s\n", port_,
                clientsCapacity(), gSettings.user[0] ? "要登录" : "允许匿名");
}

void Broker::stop() {
  for (int i = 0; i < (int)clientCount_; ++i) {
    if (C(i).used) {
      C(i).sock.stop();
      C(i).used = false;
      C(i).authed = false;
    }
  }
  freeSlots();
  server_.end();
  running_ = false;
}

void Broker::freeSlots() {
  for (size_t i = 0; i < clientCount_; ++i) delete clients_[i];
  free(clients_);
  clients_ = nullptr;
  clientCount_ = 0;
  clientCap_ = 0;
}

// 找一个空闲槽位，没有就扩容；内存不够返回 nullptr。
// 槽位里的 Client 对象复用，不随断开释放，这样索引在遍历途中也不会失效。
Broker::Client* Broker::allocSlot() {
  for (size_t i = 0; i < clientCount_; ++i) {
    if (clients_[i] && !clients_[i]->used) return clients_[i];
  }
  if (clientCount_ >= clientCap_) {
    const size_t cap = clientCap_ ? clientCap_ * 2 : 8;
    Client** p = (Client**)realloc(clients_, cap * sizeof(Client*));
    if (!p) {
      Serial.printf("[MQTT] 扩容客户端表失败（要 %u 个槽位，堆剩 %u）\n", (unsigned)cap,
                    (unsigned)ESP.getFreeHeap());
      return nullptr;
    }
    for (size_t i = clientCap_; i < cap; ++i) p[i] = nullptr;
    clients_ = p;
    clientCap_ = cap;
  }
  Client* c = new (std::nothrow) Client();
  if (!c) {
    Serial.printf("[MQTT] 新客户端分配失败（每台约 %u 字节，堆剩 %u）\n", (unsigned)sizeof(Client),
                  (unsigned)ESP.getFreeHeap());
    return nullptr;
  }
  clients_[clientCount_++] = c;
  return c;
}

int Broker::clientsConnected() const {
  int n = 0;
  for (int i = 0; i < (int)clientCount_; ++i)
    if (C(i).used && C(i).authed) ++n;
  return n;
}

int Broker::clientsTotal() const {
  int n = 0;
  for (int i = 0; i < (int)clientCount_; ++i)
    if (C(i).used) ++n;
  return n;
}

// socket 预算：芯片总数 - 监听口（80 / MQTT）- 留给网页的并发请求。
// 这不是写死的台数，换芯片或改 sdkconfig 后会自动跟着变
int Broker::clientsCapacity() const {
  int cap = CONFIG_LWIP_MAX_SOCKETS - 2 - MQTT_SOCKET_RESERVE_WEB;
  return cap > 0 ? cap : 1;
}


String Broker::status() const {
  if (!running_) {
    String s = "服务端未启动";
    if (lastError_[0]) {  // 启动失败留了原因（端口被占），一起显示出来
      s += "：";
      s += lastError_;
    }
    return s;
  }
  String s = "服务端 :";
  s += port_;
  s += " · 已连 ";
  s += clientsConnected();
  s += " 台（上限 ";
  s += clientsCapacity();
  s += "）";
  return s;
}

// ---------------- 主循环 ----------------

void Broker::loop() {
  if (!running_) return;
  acceptNew();
  const uint32_t now = millis();
  for (int i = 0; i < (int)clientCount_; ++i) {
    if (C(i).used) serviceClient(i, now);
  }
}

void Broker::acceptNew() {
  for (int guard = 0; guard < 2; ++guard) {  // 一次 loop 最多接两个，别把主循环卡住
    WiFiClient nc = server_.available();
    if (!nc) return;

    // 只按芯片能力兜底：socket 或内存真的不够了才拒，不看「已经连了几台」
    if (clientsTotal() >= clientsCapacity()) {
      Serial.printf("[MQTT] 拒绝新连接 %s：socket 预算已用完（%d 个，芯片共 %d 个）\n",
                    nc.remoteIP().toString().c_str(), clientsCapacity(), CONFIG_LWIP_MAX_SOCKETS);
      nc.stop();
      continue;
    }
    if (ESP.getFreeHeap() < MQTT_MIN_FREE_HEAP) {
      Serial.printf("[MQTT] 拒绝新连接 %s：内存不足（堆剩 %u）\n",
                    nc.remoteIP().toString().c_str(), (unsigned)ESP.getFreeHeap());
      nc.stop();
      continue;
    }

    Client* pc = allocSlot();
    if (!pc) {
      nc.stop();
      continue;
    }
    Client& c = *pc;
    c.sock = nc;
    c.sock.setNoDelay(true);
    c.used = true;
    c.authed = false;
    c.willArmed = false;
    c.ver = 4;  // CONNECT 里再按实际协议级别改写
    c.clientId[0] = 0;
    c.willTopic[0] = 0;
    c.willMsg[0] = 0;
    c.willRetain = false;
    c.keepAlive = 0;
    c.lastRx = millis();
    c.subCount = 0;
    c.hdr = 0;
    c.phase = 0;
    c.remain = 0;
    c.mul = 1;
    c.lenBytes = 0;
    c.got = 0;
    Serial.printf("[MQTT] 新连接 %s（第 %d 个，共 %u 个槽位）\n", c.sock.remoteIP().toString().c_str(),
                  clientsTotal(), (unsigned)clientCount_);
  }
}

void Broker::serviceClient(int i, uint32_t now) {
  Client& c = C(i);
  if (!c.sock.connected() && !c.sock.available()) {
    dropClient(i, true, "网络断开");
    return;
  }
  int budget = 1536;  // 每次最多读这么多，超大报文分几次读完
  while (budget-- > 0 && c.sock.available()) {
    int b = c.sock.read();
    if (b < 0) break;
    c.lastRx = now;
    feedByte(i, (uint8_t)b);
    if (!C(i).used) return;  // 报文处理里把连接关了
  }
  if (!C(i).used) return;
  if (c.authed && c.keepAlive && (now - c.lastRx) > (uint32_t)c.keepAlive * 1500UL) {
    dropClient(i, true, "keepalive 超时");
  }
}

void Broker::feedByte(int i, uint8_t b) {
  Client& c = C(i);
  switch (c.phase) {
    case 0:  // 固定头
      c.hdr = b;
      c.phase = 1;
      c.remain = 0;
      c.mul = 1;
      c.lenBytes = 0;
      c.got = 0;
      break;
    case 1:  // 剩余长度（变长编码）
      c.remain += (uint32_t)(b & 0x7F) * c.mul;
      c.mul *= 128;
      if (++c.lenBytes > 4) {
        dropClient(i, true, "剩余长度格式错");
        return;
      }
      if (!(b & 0x80)) {
        if (c.remain > MQTT_BODY_MAX) {
          dropClient(i, true, "报文太长");
          return;
        }
        c.phase = 2;
        if (c.remain == 0) {
          c.phase = 0;
          dispatch(i);
        }
      }
      break;
    default:  // 消息体
      c.body[c.got++] = b;
      if (c.got >= c.remain) {
        c.phase = 0;
        dispatch(i);
      }
      break;
  }
}

void Broker::dispatch(int i) {
  Client& c = C(i);
  switch (c.hdr >> 4) {
    case T_CONNECT:
      onConnect(i);
      break;
    case T_PUBLISH:
      onPublish(i);
      break;
    case T_SUBSCRIBE:
      onSubscribe(i);
      break;
    case T_UNSUBSCRIBE:
      onUnsubscribe(i);
      break;
    case T_PINGREQ: {
      const uint8_t rsp[2] = {T_PINGRESP << 4, 0};
      c.sock.write(rsp, 2);
      break;
    }
    case T_DISCONNECT:
      dropClient(i, false, "客户端主动断开");
      break;
    case T_PUBREL: {  // QoS 2 的第三步：回 PUBCOMP，这条消息的事务就结束了
      const uint16_t pid = c.remain >= 2 ? (uint16_t)(c.body[0] << 8 | c.body[1]) : 0;
      const uint8_t ack[4] = {T_PUBCOMP << 4, 2, (uint8_t)(pid >> 8), (uint8_t)pid};
      c.sock.write(ack, 4);
      c.qos2Pending = false;
      break;
    }
    case T_PUBACK:
    case T_PUBREC:
    case T_PUBCOMP:
    case T_PINGRESP:
    case T_AUTH:
      break;  // 我们只发 QoS 0，收到也忽略；AUTH 只在 MQTT 5 的增强认证里出现
    default:
      dropClient(i, true, "不支持的报文类型");
      break;
  }
}

// ---------------- CONNECT ----------------

void Broker::onConnect(int i) {
  Client& c = C(i);
  Cur cur{c.body, c.remain};
  const char* proto = nullptr;
  size_t plen = 0;
  uint8_t level = 0, flags = 0;
  uint16_t keep = 0;

  // 拒绝时的返回码两种格式不一样：3.x 是 1~5，5.0 是 0x8x 系列，而且 CONNACK 多一个属性长度字节
  auto refuse = [&](uint8_t rcV4, uint8_t rcV5, const char* why) {
    if (c.ver == 5) {
      const uint8_t ack[5] = {T_CONNACK << 4, 3, 0, rcV5, 0};
      c.sock.write(ack, 5);
    } else {
      const uint8_t ack[4] = {T_CONNACK << 4, 2, 0, rcV4};
      c.sock.write(ack, 4);
    }
    Serial.printf("[MQTT] 拒绝 %s：%s\n", c.sock.remoteIP().toString().c_str(), why);
    dropClient(i, false, why);
  };

  if (!cur.str(&proto, &plen)) {
    refuse(0x01, 0x81, "CONNECT 报文不完整");
    return;
  }
  // 协议名 + 级别：MQTT/4 与 MQTT/5，或老式的 MQIsdp/3
  const bool isMqtt = (plen == 4 && memcmp(proto, "MQTT", 4) == 0);
  const bool isOld = (plen == 6 && memcmp(proto, "MQIsdp", 6) == 0);
  if (!isMqtt && !isOld) {
    refuse(0x01, 0x84, "协议名不是 MQTT");
    return;
  }
  if (!cur.u8(&level)) {
    refuse(0x01, 0x81, "CONNECT 报文不完整");
    return;
  }
  if ((isMqtt && level != 4 && level != 5) || (isOld && level != 3)) {
    char why[64];
    // 只认这几种，别的版本按规范回「协议版本不支持」
    snprintf(why, sizeof(why), "不支持的协议版本 %u（支持 3.1 / 3.1.1 / 5.0）", level);
    refuse(0x01, 0x84, why);
    return;
  }
  c.ver = level;

  if (!cur.u8(&flags) || !cur.u16(&keep)) {
    refuse(0x01, 0x81, "CONNECT 报文不完整");
    return;
  }
  if (flags & 0x01) {  // bit0 是保留位，必须为 0
    refuse(0x01, 0x81, "CONNECT 标志位非法");
    return;
  }
  if (c.ver == 5 && !cur.skipProps()) {  // 5.0 的 CONNECT 属性（会话过期、最大报文长度等），跳过
    refuse(0x01, 0x81, "CONNECT 属性不完整");
    return;
  }

  const char* cid = nullptr;
  size_t cidLen = 0;
  if (!cur.str(&cid, &cidLen)) {
    refuse(0x02, 0x85, "没有 clientId");
    return;
  }
  copyN(c.clientId, sizeof(c.clientId), cid, cidLen);

  if (flags & 0x04) {  // 遗嘱
    if (c.ver == 5 && !cur.skipProps()) {  // 5.0 还多一段遗嘱属性
      refuse(0x01, 0x81, "遗嘱属性不完整");
      return;
    }
    const char* wt = nullptr;
    size_t wtl = 0;
    const char* wm = nullptr;
    size_t wml = 0;
    if (!cur.str(&wt, &wtl) || !cur.str(&wm, &wml)) {
      refuse(0x01, 0x81, "遗嘱字段不完整");
      return;
    }
    copyN(c.willTopic, sizeof(c.willTopic), wt, wtl);
    copyN(c.willMsg, sizeof(c.willMsg), wm, wml);
    c.willRetain = (flags & 0x20) != 0;
  }

  const char* user = nullptr;
  size_t userLen = 0;
  const char* pass = nullptr;
  size_t passLen = 0;
  if (flags & 0x80) {
    if (!cur.str(&user, &userLen)) {
      refuse(0x01, 0x81, "用户名字段不完整");
      return;
    }
  }
  if (flags & 0x40) {
    if (!cur.str(&pass, &passLen)) {
      refuse(0x01, 0x81, "密码字段不完整");
      return;
    }
  }

  // 设置了账号才校验；没设置就是允许匿名。密码可以为空（只验用户名）
  if (gSettings.user[0]) {
    const bool userOk = user && constEq(user, userLen, gSettings.user);
    const bool passOk = gSettings.pass[0] ? (pass && constEq(pass, passLen, gSettings.pass))
                                           : (!pass || passLen == 0);
    if (!userOk || !passOk) {
      refuse(0x04, 0x86, "用户名或密码不对");
      return;
    }
  }

  // 同一个 clientId 又连进来：踢掉旧的（MQTT 3.1.1 第 3.1.4 节）
  for (int j = 0; j < (int)clientCount_; ++j) {
    if (j != i && C(j).used && C(j).authed &&
        strcmp(C(j).clientId, c.clientId) == 0) {
      dropClient(j, true, "同 clientId 重复登录");
    }
  }

  c.authed = true;
  c.willArmed = c.willTopic[0] != 0;
  c.keepAlive = keep > MQTT_KEEPALIVE_LIMIT_S ? MQTT_KEEPALIVE_LIMIT_S : keep;
  if (c.ver == 5) {
    const uint8_t ack[5] = {T_CONNACK << 4, 3, 0, 0, 0};  // 5.0 要带属性长度 0
    c.sock.write(ack, 5);
  } else {
    const uint8_t ack[4] = {T_CONNACK << 4, 2, 0, 0};
    c.sock.write(ack, 4);
  }
  const char* verName = c.ver == 3 ? "3.1" : (c.ver == 5 ? "5.0" : "3.1.1");
  Serial.printf("[MQTT] 客户端上线 %s（%s）协议 %s keepalive %us\n", c.clientId,
                c.sock.remoteIP().toString().c_str(), verName, c.keepAlive);
}

// ---------------- SUBSCRIBE / UNSUBSCRIBE ----------------

bool Broker::addSub(Client& c, const char* filter) {
  for (int k = 0; k < c.subCount; ++k) {
    if (strcmp(c.subs[k], filter) == 0) return true;  // 已经有了
  }
  if (c.subCount >= MQTT_SUBS_PER_CLIENT) return false;
  copyN(c.subs[c.subCount], MQTT_SUB_LEN, filter, strlen(filter));
  ++c.subCount;
  return true;
}

bool Broker::subMatches(const Client& c, const char* topic) {
  for (int k = 0; k < c.subCount; ++k) {
    if (semi::topic::matches(c.subs[k], topic)) return true;
  }
  return false;
}

void Broker::onSubscribe(int i) {
  Client& c = C(i);
  if (!c.authed) {
    dropClient(i, true, "还没 CONNECT 就订阅");
    return;
  }
  Cur cur{c.body, c.remain};
  uint16_t pid = 0;
  if (!cur.u16(&pid)) {
    dropClient(i, true, "SUBSCRIBE 报文不完整");
    return;
  }
  if (c.ver == 5 && !cur.skipProps()) {  // 5.0 的订阅属性（比如订阅标识）
    dropClient(i, true, "SUBSCRIBE 属性不完整");
    return;
  }
  uint8_t results[8];
  char added[8][MQTT_SUB_LEN];
  int n = 0, addedCount = 0;
  while (cur.i < cur.n && n < 8) {
    const char* f = nullptr;
    size_t fl = 0;
    uint8_t qos = 0;
    if (!cur.str(&f, &fl) || !cur.u8(&qos)) break;
    char filter[MQTT_SUB_LEN];
    // 超长就判失败（SUBACK 回 0x80），别截断——截断后变成另一个主题，
    // 表面上订阅成功、其实永远收不到消息
    if (fl == 0 || fl >= sizeof(filter)) {
      results[n++] = 0x80;
      Serial.printf("[MQTT] %s 订阅失败：过滤器 %u 字节，上限 %u 字节\n", c.clientId, (unsigned)fl,
                    (unsigned)(sizeof(filter) - 1));
      continue;
    }
    copyN(filter, sizeof(filter), f, fl);
    const bool isNew = [&] {
      for (int k = 0; k < c.subCount; ++k)
        if (strcmp(c.subs[k], filter) == 0) return false;
      return true;
    }();
    const bool ok = addSub(c, filter);
    if (ok && isNew && addedCount < 8) {
      copyN(added[addedCount++], MQTT_SUB_LEN, filter, strlen(filter));
    }
    results[n++] = ok ? 0 : 0x80;
    Serial.printf("[MQTT] %s 订阅 %s\n", c.clientId, filter);
  }

  // 5.0 的 SUBACK 在 packetId 后面多一个属性长度
  uint8_t body[2 + 1 + 8];
  size_t bn = 0;
  body[bn++] = (uint8_t)(pid >> 8);
  body[bn++] = (uint8_t)pid;
  if (c.ver == 5) body[bn++] = 0;
  for (int k = 0; k < n; ++k) body[bn++] = results[k];
  uint8_t rsp[2 + sizeof(body)];
  rsp[0] = T_SUBACK << 4;
  rsp[1] = (uint8_t)bn;
  memcpy(rsp + 2, body, bn);
  c.sock.write(rsp, 2 + bn);

  // 新订阅要把对应的保留消息补一份（进料器一重连就能马上看到状态）
  for (int k = 0; k < addedCount; ++k) {
    for (int r = 0; r < MAX_RETAINED; ++r) {
      Retained& rt = retained_[r];
      if (!rt.topic[0]) continue;
      if (!semi::topic::matches(added[k], rt.topic)) continue;
      sendPublish(c, rt.topic, rt.payload, rt.len, true);
    }
  }
}

void Broker::onUnsubscribe(int i) {
  Client& c = C(i);
  Cur cur{c.body, c.remain};
  uint16_t pid = 0;
  if (!cur.u16(&pid)) {
    dropClient(i, true, "UNSUBSCRIBE 报文不完整");
    return;
  }
  if (c.ver == 5 && !cur.skipProps()) {
    dropClient(i, true, "UNSUBSCRIBE 属性不完整");
    return;
  }
  int count = 0;
  while (cur.i < cur.n) {
    const char* f = nullptr;
    size_t fl = 0;
    if (!cur.str(&f, &fl)) break;
    ++count;
    char filter[MQTT_SUB_LEN];
    copyN(filter, sizeof(filter), f, fl);
    for (int k = 0; k < c.subCount; ++k) {
      if (strcmp(c.subs[k], filter) == 0) {
        for (int m = k; m + 1 < c.subCount; ++m) strcpy(c.subs[m], c.subs[m + 1]);
        --c.subCount;
        break;
      }
    }
  }
  // 5.0 的 UNSUBACK 带属性长度，并且每个过滤器一个原因码；3.x 只有 packetId
  uint8_t body[2 + 1 + 8];
  size_t bn = 0;
  body[bn++] = (uint8_t)(pid >> 8);
  body[bn++] = (uint8_t)pid;
  if (c.ver == 5) {
    body[bn++] = 0;
    for (int k = 0; k < count && k < 8; ++k) body[bn++] = 0;  // 0x00 成功
  }
  uint8_t rsp[2 + sizeof(body)];
  rsp[0] = T_UNSUBACK << 4;
  rsp[1] = (uint8_t)bn;
  memcpy(rsp + 2, body, bn);
  c.sock.write(rsp, 2 + bn);
}

// ---------------- PUBLISH ----------------

void Broker::onPublish(int i) {
  Client& c = C(i);
  if (!c.authed) {
    dropClient(i, true, "还没 CONNECT 就发消息");
    return;
  }
  const uint8_t qos = (c.hdr >> 1) & 0x03;
  const bool retain = (c.hdr & 0x01) != 0;
  if (qos == 3) {
    dropClient(i, true, "QoS 3 不存在");
    return;
  }
  Cur cur{c.body, c.remain};
  const char* tp = nullptr;
  size_t tl = 0;
  if (!cur.str(&tp, &tl) || tl == 0 || tl >= MQTT_TOPIC_MAX) {
    dropClient(i, true, "主题非法");
    return;
  }
  char topic[MQTT_TOPIC_MAX];
  copyN(topic, sizeof(topic), tp, tl);

  uint16_t pid = 0;
  if (qos > 0 && !cur.u16(&pid)) {
    dropClient(i, true, "PUBLISH 报文不完整");
    return;
  }
  if (c.ver == 5 && !cur.skipProps()) {  // 5.0 的 PUBLISH 在主题（和 packetId）后面有属性块
    dropClient(i, true, "PUBLISH 属性不完整");
    return;
  }
  const size_t plen = cur.n - cur.i;
  if (plen > MQTT_BODY_MAX) {
    dropClient(i, true, "payload 太长");
    return;
  }
  memcpy(gPayload, c.body + cur.i, plen);
  gPayload[plen] = 0;

  if (qos == 1) {
    const uint8_t ack[4] = {T_PUBACK << 4, 2, (uint8_t)(pid >> 8), (uint8_t)pid};
    c.sock.write(ack, 4);
  } else if (qos == 2) {
    const uint8_t rec[4] = {T_PUBREC << 4, 2, (uint8_t)(pid >> 8), (uint8_t)pid};
    if (c.qos2Pending && c.qos2Pid == pid) {
      // 客户端没等到 PUBREC 又重发同一条（同 pid）：只补一个 PUBREC，不重复投递
      c.sock.write(rec, 4);
      Serial.printf("[MQTT] %s 重发的 QoS2 报文 pid=%u，只补 PUBREC\n", c.clientId, pid);
      return;
    }
    // 收下立刻投递（不等 PUBREL 再投，省得记一堆中间状态），
    // 客户端随后发 PUBREL，我们在 dispatch 里回 PUBCOMP 收尾
    c.sock.write(rec, 4);
    c.qos2Pid = pid;
    c.qos2Pending = true;
  }
  ++inMsg_;

  if (retain) {
    if (plen == 0) {
      // 空 payload + retain = 删掉服务器上的保留消息
      for (int r = 0; r < MAX_RETAINED; ++r) {
        if (retained_[r].topic[0] && strcmp(retained_[r].topic, topic) == 0) {
          retainedBytes_ -= retained_[r].len;
          free(retained_[r].payload);
          retained_[r].payload = nullptr;
          retained_[r].len = 0;
          retained_[r].topic[0] = 0;
          break;
        }
      }
    } else {
      setRetained(topic, gPayload, plen);
    }
  }

  // 本机设备表直接吃进料器的 state / availability
  if (endsWith(topic, "/state")) {
    devicesOnState(gPayload);
  } else if (endsWith(topic, "/availability")) {
    devicesOnAvailability(gPayload);
  }

  deliver(topic, gPayload, plen, false);
}

// ---------------- 投递 ----------------

bool Broker::sendPublish(Client& c, const char* topic, const char* payload, size_t len, bool retain) {
  if (!c.used || !c.authed) return false;
  const size_t tl = strlen(topic);
  const size_t props = (c.ver == 5) ? 1 : 0;  // 5.0 的 PUBLISH 必须带属性长度（我们发空属性）
  const size_t rem = 2 + tl + props + len;
  uint8_t hdr[5];
  hdr[0] = (uint8_t)((T_PUBLISH << 4) | (retain ? 0x01 : 0x00));
  const size_t hn = 1 + putLen(hdr + 1, rem);
  const uint8_t tlen[2] = {(uint8_t)(tl >> 8), (uint8_t)tl};
  if (c.sock.write(hdr, hn) != hn) return false;
  if (c.sock.write(tlen, 2) != 2) return false;
  if (tl && c.sock.write((const uint8_t*)topic, tl) != tl) return false;
  if (props) {
    const uint8_t zero = 0;
    if (c.sock.write(&zero, 1) != 1) return false;
  }
  if (len && c.sock.write((const uint8_t*)payload, len) != len) return false;
  ++outMsg_;
  return true;
}

int Broker::deliver(const char* topic, const char* payload, size_t len, bool retain) {
  int sent = 0;
  for (int i = 0; i < (int)clientCount_; ++i) {
    Client& c = C(i);
    if (!c.used || !c.authed) continue;
    if (!subMatches(c, topic)) continue;
    if (sendPublish(c, topic, payload, len, retain)) ++sent;
  }
  return sent;
}

void Broker::setRetained(const char* topic, const char* payload, size_t len) {
  if (len > RETAINED_BYTES_MAX) return;
  // 先看是不是覆盖已有主题
  int slot = -1;
  for (int r = 0; r < MAX_RETAINED; ++r) {
    if (retained_[r].topic[0] && strcmp(retained_[r].topic, topic) == 0) {
      slot = r;
      break;
    }
  }
  if (slot < 0) {
    for (int r = 0; r < MAX_RETAINED; ++r) {
      if (!retained_[r].topic[0]) {
        slot = r;
        break;
      }
    }
  }
  if (slot < 0) {
    // 满了：淘汰最旧的一条
    uint32_t oldest = 0xFFFFFFFF;
    for (int r = 0; r < MAX_RETAINED; ++r) {
      if (retained_[r].seq < oldest) {
        oldest = retained_[r].seq;
        slot = r;
      }
    }
    if (slot < 0) return;
    retainedBytes_ -= retained_[slot].len;
    free(retained_[slot].payload);
    retained_[slot].payload = nullptr;
    retained_[slot].len = 0;
    retained_[slot].topic[0] = 0;
  }

  retainedBytes_ -= retained_[slot].len;
  char* buf = (char*)malloc(len + 1);
  if (!buf) {
    Serial.println("[MQTT] 保留消息内存不够");
    return;
  }
  memcpy(buf, payload, len);
  buf[len] = 0;
  free(retained_[slot].payload);
  retained_[slot].payload = buf;
  retained_[slot].len = (uint16_t)len;
  retained_[slot].seq = ++seq_;
  copyN(retained_[slot].topic, MQTT_TOPIC_MAX, topic, strlen(topic));
  retainedBytes_ += len;

  // 总量超了就按最旧的淘汰
  while (retainedBytes_ > RETAINED_BYTES_MAX) {
    int victim = -1;
    uint32_t oldest = 0xFFFFFFFF;
    for (int r = 0; r < MAX_RETAINED; ++r) {
      if (retained_[r].topic[0] && retained_[r].seq != retained_[slot].seq && retained_[r].seq < oldest) {
        oldest = retained_[r].seq;
        victim = r;
      }
    }
    if (victim < 0) break;
    retainedBytes_ -= retained_[victim].len;
    free(retained_[victim].payload);
    retained_[victim].payload = nullptr;
    retained_[victim].len = 0;
    retained_[victim].topic[0] = 0;
  }
}

// ---------------- 断开 ----------------

void Broker::dropClient(int i, bool publishWill, const char* why) {
  Client& c = C(i);
  if (!c.used) return;

  char willTopic[MQTT_TOPIC_MAX];
  char willMsg[96];
  const bool hasWill = publishWill && c.authed && c.willArmed && c.willTopic[0];
  if (hasWill) {
    copyN(willTopic, sizeof(willTopic), c.willTopic, strlen(c.willTopic));
    copyN(willMsg, sizeof(willMsg), c.willMsg, strlen(c.willMsg));
  }
  const bool willRetain = c.willRetain;
  char clientId[24];
  copyN(clientId, sizeof(clientId), c.clientId, strlen(c.clientId));
  const IPAddress ip = c.sock.remoteIP();

  // 先关连接，避免 deliver 又往这个死 socket 写
  c.sock.stop();
  c.used = false;
  c.authed = false;
  c.willArmed = false;
  c.subCount = 0;

  Serial.printf("[MQTT] 客户端下线 %s（%s）：%s\n", clientId[0] ? clientId : "(未登录)",
                ip.toString().c_str(), why ? why : "");

  if (hasWill) {
    Serial.printf("[MQTT] 代发遗嘱 %s → %s\n", willTopic, willMsg);
    if (willRetain) setRetained(willTopic, willMsg, strlen(willMsg));
    if (endsWith(willTopic, "/availability")) devicesOnAvailability(willMsg);
    deliver(willTopic, willMsg, strlen(willMsg), false);
  }
}

// ---------------- 对外发布（网页按钮） ----------------

bool Broker::publish(const char* topic, const char* payload, size_t len, bool retain, String& err,
                     int* delivered) {
  if (delivered) *delivered = 0;
  if (!running_) {
    err = "MQTT 服务端没启动";
    if (lastError_[0]) {
      err += "：";
      err += lastError_;
    }
    return false;
  }
  if (len > MQTT_BODY_MAX) {
    err = "消息太长";
    return false;
  }
  if (retain) setRetained(topic, payload, len);
  const int n = deliver(topic, payload, len, false);
  if (delivered) *delivered = n;
  if (n == 0) {
    // 一台都没送到就别报成功，否则网页上「已发送」是假的
    err = "没有客户端订阅 ";
    err += topic;
    Serial.printf("[MQTT] ✗ %s %s：没有订阅者，消息没发出去\n", topic, payload);
    return false;
  }
  Serial.printf("[MQTT] → %s %s（%d 台）\n", topic, payload, n);
  return true;
}

}  // namespace ctrl
