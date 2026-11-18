// 内置的 MQTT 服务端（broker），同时兼容 3.1（MQIsdp/level 3）、3.1.1（level 4）、5.0（level 5）
//
// ESP32-C3 自己当服务器，进料器直接连它，不需要额外的 Mosquitto / Home Assistant。
// 实现的部分：CONNECT（含用户名密码校验）、SUBSCRIBE / UNSUBSCRIBE（+ # 通配符）、
// PUBLISH（QoS 0/1 完整；QoS 2 收下就回 PUBREC 并投递，再回 PUBCOMP，不做重发补齐）、
// 保留消息、遗嘱（LWT，掉电时代发 offline）、PINGREQ。
// 没做的：持久会话（cleanSession=0 按 1 处理）、共享订阅。进料器只用 QoS 0/1，够用。
#pragma once
#include <Arduino.h>
#include <WiFi.h>

#include "config.h"

// 芯片能同时开多少 socket：这个常量来自 IDF 的 sdkconfig.h（ESP32-C3 = 16），
// 编译进静态库，运行时改不了，所以只能据此推算预算，不能凭空放大
#ifndef CONFIG_LWIP_MAX_SOCKETS
#define CONFIG_LWIP_MAX_SOCKETS 16
#endif

namespace ctrl {

class Broker {
 public:
  void begin();   // 按当前设置里的端口开始监听
  void stop();
  void loop();

  bool running() const { return running_; }
  uint16_t port() const { return port_; }
  int clientsConnected() const;   // 已经完成 CONNECT 的客户端数
  int clientsTotal() const;       // 占用的连接数（含还没发 CONNECT 的）
  int clientsCapacity() const;    // 按 socket 数算出来的上限（仅用于显示，不是硬编码的）
  String status() const;          // 网页上显示的一句话

  // 发给所有订阅者（网页点按钮走这里）。
  // delivered 返回实际送达的客户端数，为 0 时按失败处理（err 里说明没人订阅），
  // 免得网页上显示「已发送」其实谁都没收到。
  bool publish(const char* topic, const char* payload, size_t len, bool retain, String& err,
               int* delivered = nullptr);

  uint32_t inMessages() const { return inMsg_; }
  uint32_t outMessages() const { return outMsg_; }

 private:
  struct Client {
    WiFiClient sock;
    bool used = false;
    bool authed = false;    // CONNECT 通过
    bool willArmed = false; // 断开时要发遗嘱
    uint8_t ver = 4;        // 协议级别：3 = MQTT 3.1，4 = 3.1.1，5 = 5.0（决定 CONNACK/SUBACK 的格式）
    char clientId[24];
    char willTopic[MQTT_TOPIC_MAX];
    char willMsg[96];
    bool willRetain = false;
    uint16_t keepAlive = 0;
    uint32_t lastRx = 0;
    char subs[MQTT_SUBS_PER_CLIENT][MQTT_SUB_LEN];
    uint8_t subCount = 0;

    // QoS 2：收到 PUBLISH 回 PUBREC、投递一次；等客户端发 PUBREL 后再回 PUBCOMP。
    // 只记最近一个 pid 用来挡重发的报文（不做到位的两阶段状态机）
    uint16_t qos2Pid = 0;
    bool qos2Pending = false;

    // 收包状态
    uint8_t hdr = 0;
    uint8_t phase = 0;  // 0 = 等固定头，1 = 等剩余长度，2 = 收消息体
    uint32_t remain = 0;
    uint32_t mul = 1;
    uint8_t lenBytes = 0;
    uint16_t got = 0;
    uint8_t body[MQTT_BODY_MAX];
  };

  struct Retained {
    char topic[MQTT_TOPIC_MAX];
    char* payload = nullptr;
    uint16_t len = 0;
    uint32_t seq = 0;
  };

  void acceptNew();
  Client* allocSlot();  // 找一个空闲槽位，没有就扩容；返回 nullptr 表示内存不够
  void freeSlots();
  Client& C(int i) const { return *clients_[i]; }  // 槽位里的对象常驻，指针不会是空的
  void serviceClient(int i, uint32_t now);
  void feedByte(int i, uint8_t b);
  void dispatch(int i);
  void onConnect(int i);
  void onSubscribe(int i);
  void onUnsubscribe(int i);
  void onPublish(int i);
  void dropClient(int i, bool publishWill, const char* why);

  bool addSub(Client& c, const char* filter);
  bool subMatches(const Client& c, const char* topic);
  bool sendPublish(Client& c, const char* topic, const char* payload, size_t len, bool retain);
  int deliver(const char* topic, const char* payload, size_t len, bool retain);  // 返回送达台数
  void setRetained(const char* topic, const char* payload, size_t len);
  void sendRetainedFor(int i);
  bool checkAuth(const char* user, const char* pass) const;

  WiFiServer server_{MQTT_PORT_DEFAULT, (uint8_t)MQTT_LISTEN_BACKLOG};
  bool running_ = false;
  uint16_t port_ = MQTT_PORT_DEFAULT;
  char lastError_[64] = {0};  // 最近一次启动失败的原因（端口被占之类），给网页显示用

  // 客户端表按需扩容：槽位数没有编译期上限，来一台分配一台。
  // 槽位里的对象复用（不随连接释放），所以槽位指针不会变成空，索引始终稳定。
  Client** clients_ = nullptr;
  size_t clientCount_ = 0;  // 已分配的槽位数
  size_t clientCap_ = 0;    // 指针数组容量

  Retained retained_[MAX_RETAINED];
  size_t retainedBytes_ = 0;
  uint32_t seq_ = 0;
  uint32_t inMsg_ = 0;
  uint32_t outMsg_ = 0;
};

extern Broker gBroker;

}  // namespace ctrl
