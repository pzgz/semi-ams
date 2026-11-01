// 最小的 MQTT 3.1.1 报文编码 / 流式解析（纯 C++，可在电脑上测试）
//
// 只实现连接拓竹打印机需要的部分：CONNECT、SUBSCRIBE、PUBLISH（QoS 0）、PINGREQ、DISCONNECT，
// 以及解析 CONNACK、SUBACK、PUBLISH、PINGRESP。
// 收到的 PUBLISH 消息体按字节交给回调，不需要把整条消息放进内存（打印机的状态消息可能有十几 KB）。
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace semi {
namespace mqtt {

enum : uint8_t {
  CONNECT = 1,
  CONNACK = 2,
  PUBLISH = 3,
  PUBACK = 4,
  SUBSCRIBE = 8,
  SUBACK = 9,
  PINGREQ = 12,
  PINGRESP = 13,
  DISCONNECT = 14,
};

// ---------- 编码：写入 buf，返回长度；空间不够返回 0 ----------
namespace detail {
inline size_t putLen(uint8_t* p, size_t len) {  // 剩余长度（变长编码）
  size_t n = 0;
  do {
    uint8_t b = len % 128;
    len /= 128;
    if (len) b |= 0x80;
    p[n++] = b;
  } while (len && n < 4);
  return n;
}
inline size_t putStr(uint8_t* p, const char* s, size_t len) {
  p[0] = (uint8_t)(len >> 8);
  p[1] = (uint8_t)len;
  memcpy(p + 2, s, len);
  return len + 2;
}
// 先写到 body（从 buf+5 开始，给固定头留位置），再把固定头挪到紧挨着 body 的前面
inline size_t finish(uint8_t* buf, uint8_t header, size_t bodyLen) {
  uint8_t tmp[5];
  tmp[0] = header;
  size_t h = 1 + putLen(tmp + 1, bodyLen);
  memmove(buf + h, buf + 5, bodyLen);
  memcpy(buf, tmp, h);
  return h + bodyLen;
}
}  // namespace detail

inline size_t buildConnect(uint8_t* buf, size_t cap, const char* clientId, const char* user, const char* pass,
                           uint16_t keepAliveS) {
  const size_t lc = strlen(clientId), lu = user ? strlen(user) : 0, lp = pass ? strlen(pass) : 0;
  const size_t body = 10 + 2 + lc + (user ? 2 + lu : 0) + (pass ? 2 + lp : 0);
  if (body + 5 > cap) return 0;
  uint8_t* p = buf + 5;
  const uint8_t proto[] = {0, 4, 'M', 'Q', 'T', 'T', 4};
  memcpy(p, proto, sizeof(proto));
  p += sizeof(proto);
  uint8_t flags = 0x02;  // clean session
  if (user) flags |= 0x80;
  if (pass) flags |= 0x40;
  *p++ = flags;
  *p++ = (uint8_t)(keepAliveS >> 8);
  *p++ = (uint8_t)keepAliveS;
  p += detail::putStr(p, clientId, lc);
  if (user) p += detail::putStr(p, user, lu);
  if (pass) p += detail::putStr(p, pass, lp);
  return detail::finish(buf, CONNECT << 4, body);
}

inline size_t buildSubscribe(uint8_t* buf, size_t cap, uint16_t packetId, const char* topic, uint8_t qos) {
  const size_t lt = strlen(topic);
  const size_t body = 2 + 2 + lt + 1;
  if (body + 5 > cap) return 0;
  uint8_t* p = buf + 5;
  *p++ = (uint8_t)(packetId >> 8);
  *p++ = (uint8_t)packetId;
  p += detail::putStr(p, topic, lt);
  *p++ = qos;
  return detail::finish(buf, (SUBSCRIBE << 4) | 0x02, body);
}

inline size_t buildPublish(uint8_t* buf, size_t cap, const char* topic, const char* payload, size_t len) {
  const size_t lt = strlen(topic);
  const size_t body = 2 + lt + len;
  if (body + 5 > cap) return 0;
  uint8_t* p = buf + 5;
  p += detail::putStr(p, topic, lt);
  memcpy(p, payload, len);
  return detail::finish(buf, PUBLISH << 4, body);  // QoS 0
}

inline size_t buildPubAck(uint8_t* buf, size_t cap, uint16_t packetId) {
  if (cap < 4) return 0;
  buf[0] = PUBACK << 4;
  buf[1] = 2;
  buf[2] = (uint8_t)(packetId >> 8);
  buf[3] = (uint8_t)packetId;
  return 4;
}

inline size_t buildPingReq(uint8_t* buf, size_t cap) {
  if (cap < 2) return 0;
  buf[0] = PINGREQ << 4;
  buf[1] = 0;
  return 2;
}

inline size_t buildDisconnect(uint8_t* buf, size_t cap) {
  if (cap < 2) return 0;
  buf[0] = DISCONNECT << 4;
  buf[1] = 0;
  return 2;
}

// ---------- 解析：逐字节输入 ----------
class Listener {
 public:
  virtual ~Listener() {}
  virtual void onConnAck(uint8_t returnCode) = 0;
  virtual void onSubAck(uint16_t packetId, uint8_t result) = 0;
  virtual void onPingResp() = 0;
  // PUBLISH：先给出主题，然后逐段给出消息体，最后 end；qos>0 时需要回 PUBACK
  virtual void onPublishBegin(const char* topic, size_t payloadLen) = 0;
  virtual void onPublishData(const uint8_t* data, size_t n) = 0;
  virtual void onPublishEnd(uint8_t qos, uint16_t packetId) = 0;
};

class Reader {
 public:
  static const size_t kTopicMax = 64;

  explicit Reader(Listener* l = nullptr) : l_(l) {}
  void setListener(Listener* l) { l_ = l; }
  void reset() {
    st_ = St::Header;
    error_ = false;
    stopped_ = false;
  }
  bool error() const { return error_; }
  // 在回调里断开连接后调用：本次 feed 剩下的数据不再处理
  void stop() { stopped_ = true; }

  // 输入一段数据，返回处理的字节数（出错时停止）
  size_t feed(const uint8_t* d, size_t n) {
    size_t i = 0;
    while (i < n && !error_ && !stopped_) {
      if (st_ == St::Payload) {  // 消息体整段转交，减少回调次数
        size_t take = n - i;
        if (take > left_) take = left_;
        if (take && l_) l_->onPublishData(d + i, take);
        i += take;
        left_ -= take;
        if (!left_) endPublish();
        continue;
      }
      byte(d[i++]);
    }
    return i;
  }

 private:
  enum class St : uint8_t { Header, Len, TopicHi, TopicLo, Topic, PidHi, PidLo, Payload, Body };

  void byte(uint8_t b) {
    switch (st_) {
      case St::Header:
        type_ = b >> 4;
        flags_ = b & 0x0F;
        if (type_ == PUBLISH && ((flags_ >> 1) & 3) == 3) {  // QoS 3 不存在
          error_ = true;
          return;
        }
        len_ = 0;
        mul_ = 1;
        lenBytes_ = 0;
        st_ = St::Len;
        break;
      case St::Len:
        len_ += (b & 0x7F) * mul_;
        mul_ *= 128;
        if (++lenBytes_ > 4) {
          error_ = true;
          return;
        }
        if (!(b & 0x80)) startBody();
        break;
      case St::TopicHi:
        topicLen_ = (size_t)b << 8;
        left_--;
        st_ = St::TopicLo;
        break;
      case St::TopicLo:
        topicLen_ |= b;
        left_--;
        if (topicLen_ > left_) {
          error_ = true;
          return;
        }
        topicPos_ = 0;
        topic_[0] = 0;
        st_ = topicLen_ ? St::Topic : afterTopic();
        if (st_ == St::Payload) beginPayload();
        break;
      case St::Topic:
        if (topicPos_ < kTopicMax - 1) topic_[topicPos_] = (char)b;
        topicPos_++;
        left_--;
        if (topicPos_ == topicLen_) {
          topic_[topicPos_ < kTopicMax ? topicPos_ : kTopicMax - 1] = 0;
          st_ = afterTopic();
          if (st_ == St::Payload) beginPayload();
        }
        break;
      case St::PidHi:
        if (left_ < 2) {
          error_ = true;
          return;
        }
        pid_ = (uint16_t)b << 8;
        left_--;
        st_ = St::PidLo;
        break;
      case St::PidLo:
        pid_ |= b;
        left_--;
        st_ = St::Payload;
        beginPayload();
        break;
      case St::Body:
        if (bodyPos_ < sizeof(body_)) body_[bodyPos_] = b;
        bodyPos_++;
        if (--left_ == 0) endBody();
        break;
      case St::Payload:
        break;  // feed() 里整段处理
    }
  }

  St afterTopic() { return ((flags_ >> 1) & 3) ? St::PidHi : St::Payload; }

  void startBody() {
    left_ = len_;
    if (type_ == PUBLISH) {
      pid_ = 0;
      if (left_ < 2) {
        error_ = true;
        return;
      }
      st_ = St::TopicHi;
      return;
    }
    bodyPos_ = 0;
    if (!left_) {
      endBody();
      return;
    }
    st_ = St::Body;
  }

  void beginPayload() {
    if (l_) l_->onPublishBegin(topic_, left_);
    if (!left_) endPublish();
  }

  void endPublish() {
    if (l_) l_->onPublishEnd((flags_ >> 1) & 3, pid_);
    st_ = St::Header;
  }

  void endBody() {
    if (l_) {
      if (type_ == CONNACK && bodyPos_ >= 2) l_->onConnAck(body_[1]);
      else if (type_ == SUBACK && bodyPos_ >= 3) l_->onSubAck((uint16_t)(body_[0] << 8 | body_[1]), body_[2]);
      else if (type_ == PINGRESP) l_->onPingResp();
    }
    st_ = St::Header;
  }

  Listener* l_;
  St st_ = St::Header;
  bool error_ = false;
  bool stopped_ = false;
  uint8_t type_ = 0, flags_ = 0;
  size_t len_ = 0, mul_ = 1, left_ = 0;
  uint8_t lenBytes_ = 0;
  size_t topicLen_ = 0, topicPos_ = 0;
  char topic_[kTopicMax] = {};
  uint16_t pid_ = 0;
  uint8_t body_[8] = {};
  size_t bodyPos_ = 0;
};

}  // namespace mqtt
}  // namespace semi
