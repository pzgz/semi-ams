// 局域网 MQTT 相关的测试：topic 前缀、设备 ID、控制命令解析、带遗嘱的 CONNECT、保留标志
// 由 test_main.cpp 包含（共用 CHECK / title）
#include "lanmqtt_proto.h"
#include "mqtt_codec.h"

static void test_lan_prefix() {
  title("局域网 MQTT topic 前缀：留空恢复默认、结尾补 /、拒绝通配符和会破坏网页的字符");
  char out[33] = "old/";
  CHECK(lan::normalizePrefix("", out, sizeof(out)) && !std::strcmp(out, "semiams/"));
  CHECK(lan::normalizePrefix("home/feeder", out, sizeof(out)) && !std::strcmp(out, "home/feeder/"));
  CHECK(lan::normalizePrefix("home/feeder/", out, sizeof(out)) && !std::strcmp(out, "home/feeder/"));
  CHECK(lan::normalizePrefix("家里/进料器", out, sizeof(out)) && !std::strcmp(out, "家里/进料器/"));
  const char* bad[] = {"a+b", "a#", "a b", "a\tb", "a\"b", "a'b", "a<b", "a>b", "a&b", "a\\b"};
  for (const char* b : bad) {
    std::strcpy(out, "keep/");
    CHECK(!lan::normalizePrefix(b, out, sizeof(out)) && !std::strcmp(out, "keep/"));  // 不修改
  }
  // 长度：31 个字符 + 补的 / 正好装满 33 字节；32 个字符不带 / 装不下，带 / 刚好
  std::string s31(31, 'a'), s32(32, 'a');
  CHECK(lan::normalizePrefix(s31.c_str(), out, sizeof(out)) && std::strlen(out) == 32 && out[31] == '/');
  CHECK(!lan::normalizePrefix(s32.c_str(), out, sizeof(out)));
  std::string s32slash = s31 + "/";
  CHECK(lan::normalizePrefix(s32slash.c_str(), out, sizeof(out)) && std::strlen(out) == 32);
}

static void test_lan_device_id() {
  title("局域网 MQTT 设备 ID：MAC 去掉冒号、小写；MAC 比较不区分大小写和分隔符");
  char id[13];
  lan::deviceId("84:F3:EB:dd:27:00", id, sizeof(id));
  CHECK(!std::strcmp(id, "84f3ebdd2700"));
  lan::deviceId("84-f3-eb-dd-27-00", id, sizeof(id));
  CHECK(!std::strcmp(id, "84f3ebdd2700"));
  lan::deviceId("", id, sizeof(id));
  CHECK(id[0] == 0);
  lan::deviceId("84:f3:eb:dd:27", id, sizeof(id));  // 少一段
  CHECK(id[0] == 0);
  lan::deviceId("84:f3:eb:dd:27:00:11", id, sizeof(id));  // 多一段
  CHECK(id[0] == 0);
  lan::deviceId("84:f3:eb:dd:27:0g", id, sizeof(id));  // 不是十六进制
  CHECK(id[0] == 0);
  CHECK(lan::sameMac("84:F3:EB:DD:27:00", "84:f3:eb:dd:27:00"));
  CHECK(lan::sameMac("84f3ebdd2700", "84:f3:eb:dd:27:00"));
  CHECK(!lan::sameMac("84:f4:eb:dd:27:00", "84:f3:eb:dd:27:00"));
  CHECK(!lan::sameMac("", ""));  // 取不到本机 MAC 时谁都不匹配
}

static void test_lan_parse_control() {
  title("局域网 MQTT 控制命令：只看根对象的 mac 和第一个动作；值接受 on/off、true/false、1/0");
  lan::Command c;
  const char* m1 = "{\"ip\":\"192.168.5.222\",\"mac\":\"84:f3:eb:dd:27:00\",\"start\":\"on\"}";
  CHECK(lan::parseControl(m1, std::strlen(m1), c));
  CHECK(!std::strcmp(c.mac, "84:f3:eb:dd:27:00") && c.action == lan::Action::Start && c.on == 1);
  const char* m2 = " {\"mac\":\"x\", \"emg\": false}";
  CHECK(lan::parseControl(m2, std::strlen(m2), c) && c.action == lan::Action::Emergency && c.on == 0);
  const char* m3 = "{\"mac\":\"x\",\"jogf\":1}";
  CHECK(lan::parseControl(m3, std::strlen(m3), c) && c.action == lan::Action::JogForward && c.on == 1);
  const char* m4 = "{\"mac\":\"x\",\"jogr\":\"true\"}";
  CHECK(lan::parseControl(m4, std::strlen(m4), c) && c.action == lan::Action::JogReverse && c.on == 1);
  const char* m5 = "{\"mac\":\"x\",\"restart\":\"0\"}";
  CHECK(lan::parseControl(m5, std::strlen(m5), c) && c.action == lan::Action::Restart && c.on == 0);
  const char* m6 = "{\"mac\":\"x\",\"start\":\"maybe\"}";  // 值不认识
  CHECK(lan::parseControl(m6, std::strlen(m6), c) && c.action == lan::Action::Start && c.on == -1);
  const char* m7 = "{\"mac\":\"x\",\"start\":\"on\",\"restart\":\"on\"}";  // 只执行第一个
  CHECK(lan::parseControl(m7, std::strlen(m7), c) && c.action == lan::Action::Start);
  const char* m8 = "{\"ip\":\"1.2.3.4\",\"start\":\"on\"}";  // 没有 mac
  CHECK(lan::parseControl(m8, std::strlen(m8), c) && c.mac[0] == 0);
  const char* m9 = "{\"mac\":\"x\",\"note\":{\"start\":\"on\"}}";  // 嵌套里的不算
  CHECK(lan::parseControl(m9, std::strlen(m9), c) && c.action == lan::Action::None);
  const char* m10 = "{\"mac\":\"x\",\"note\":\"\\\"start\\\":\\\"on\\\"\"}";  // 字符串里的不算
  CHECK(lan::parseControl(m10, std::strlen(m10), c) && c.action == lan::Action::None);
  const char* bad[] = {"start=on", "[\"start\"]", "{\"mac\":\"x\",\"start\":\"on\"", "", "\"on\""};
  for (const char* b : bad) CHECK(!lan::parseControl(b, std::strlen(b), c));
  const char* m11 = "{\"mac\":123,\"start\":\"on\"}";  // mac 不是字符串
  CHECK(lan::parseControl(m11, std::strlen(m11), c) && c.mac[0] == 0);
}

static void test_mqtt_connect_will() {
  title("MQTT CONNECT 带遗嘱：编码符合 3.1.1；不带遗嘱时和 buildConnect 一样");
  uint8_t b[256];
  const size_t n = mqtt::buildConnectWill(b, sizeof(b), "semi-ams-abc", "u", "p", 60, "semiams/abc/availability",
                                          "{\"state\":\"offline\"}", true);
  // 固定头 0x10 + 剩余长度；可变头：00 04 MQTT 04 标志 保活
  const size_t body = 10 + (2 + 12) + (2 + 24) + (2 + 19) + (2 + 1) + (2 + 1);
  CHECK(n == 2 + body && b[0] == 0x10 && b[1] == body);
  CHECK(!std::memcmp(b + 2, "\x00\x04MQTT\x04", 7));
  CHECK(b[9] == (0x80 | 0x40 | 0x20 | 0x04 | 0x02));  // 用户名、密码、遗嘱保留、遗嘱、clean session
  CHECK(b[10] == 0 && b[11] == 60);
  // 负载顺序：客户端 ID、遗嘱 topic、遗嘱内容、用户名、密码
  size_t p = 12;
  auto str = [&](const char* want) {
    const size_t len = ((size_t)b[p] << 8) | b[p + 1];
    const bool ok = len == std::strlen(want) && !std::memcmp(b + p + 2, want, len);
    p += 2 + len;
    return ok;
  };
  CHECK(str("semi-ams-abc"));
  CHECK(str("semiams/abc/availability"));
  CHECK(str("{\"state\":\"offline\"}"));
  CHECK(str("u"));
  CHECK(str("p"));
  CHECK(p == n);
  // 不保留遗嘱、不登录
  const size_t n2 = mqtt::buildConnectWill(b, sizeof(b), "id", nullptr, nullptr, 30, "t", "m", false);
  CHECK(n2 > 0 && b[9] == (0x04 | 0x02));
  // 不带遗嘱 = buildConnect（连接打印机用）
  uint8_t c[64];
  const size_t m1 = mqtt::buildConnect(c, sizeof(c), "id", "bblp", "12345678", 60);
  const size_t m2 = mqtt::buildConnectWill(b, sizeof(b), "id", "bblp", "12345678", 60, nullptr, nullptr, true);
  CHECK(m1 == m2 && !std::memcmp(b, c, m1));
  CHECK(mqtt::buildConnectWill(b, 20, "semi-ams-abc", nullptr, nullptr, 60, "semiams/abc/availability", "x", true) == 0);
}

struct RetainRec : mqtt::Listener {
  mqtt::Reader* r = nullptr;
  std::vector<int> retained;  // 每条 PUBLISH 开始时的保留标志
  std::string payload;
  void onConnAck(uint8_t) override {}
  void onSubAck(uint16_t, uint8_t) override {}
  void onPingResp() override {}
  void onPublishBegin(const char*, size_t) override {
    retained.push_back(r->retained() ? 1 : 0);
    payload.clear();
  }
  void onPublishData(const uint8_t* d, size_t n) override { payload.append((const char*)d, n); }
  void onPublishEnd(uint8_t, uint16_t) override {}
};

static void test_mqtt_reader_retained() {
  title("MQTT 收消息：能区分服务器补发的保留消息（控制命令要忽略它，否则每次重连都会重放）");
  mqtt::Reader r;
  RetainRec rec;
  rec.r = &r;
  r.setListener(&rec);
  r.reset();
  // 保留消息（0x31 = PUBLISH | retain），QoS 0，topic "c"，内容 "x"
  const uint8_t a[] = {0x31, 4, 0, 1, 'c', 'x'};
  // 正常转发（0x30），QoS 0
  const uint8_t b[] = {0x30, 4, 0, 1, 'c', 'y'};
  // 保留 + QoS 1（0x33），带包 ID
  const uint8_t c[] = {0x33, 6, 0, 1, 'c', 0, 7, 'z'};
  r.feed(a, sizeof(a));
  r.feed(b, sizeof(b));
  r.feed(c, sizeof(c));
  CHECK(!r.error());
  CHECK(rec.retained.size() == 3 && rec.retained[0] == 1 && rec.retained[1] == 0 && rec.retained[2] == 1);
  CHECK(rec.payload == "z");
}
