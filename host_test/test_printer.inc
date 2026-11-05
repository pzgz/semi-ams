// 拓竹打印机（MQTT）相关的测试：JSON 扫描、MQTT 报文、消息解读、状态机命令
// 由 test_main.cpp 包含（共用 CHECK / title / Sim）
#include "bambu.h"
#include "json_scan.h"
#include "mqtt_codec.h"

// ---------------- JSON 扫描 ----------------
struct JsonRec {
  std::vector<std::string> items;
  static void cb(void* ctx, const JsonScanner& js, JsonScanner::Event ev, const char* v, JsonScanner::Kind) {
    auto* self = static_cast<JsonRec*>(ctx);
    std::string p;
    for (int i = 0; i < js.depth(); i++) {
      if (i) p += "/";
      p += js.key(i);
    }
    const char* tag = ev == JsonScanner::Event::Value        ? "="
                      : ev == JsonScanner::Event::ObjectBegin ? "{"
                      : ev == JsonScanner::Event::ObjectEnd   ? "}"
                      : ev == JsonScanner::Event::ArrayBegin  ? "["
                                                              : "]";
    self->items.push_back(p + tag + v);
  }
  bool has(const std::string& s) const {
    for (auto& i : items)
      if (i == s) return true;
    return false;
  }
};

static void test_json_scanner() {
  title("JSON 流式扫描：路径、数组、转义、空容器、逐字节输入、错误");
  JsonScanner js;
  JsonRec rec;
  js.setHandler(&JsonRec::cb, &rec);
  js.reset();
  const char* doc =
      "{\"print\":{\"command\":\"project_file\",\"use_ams\":false,\"x\":[],\"y\":{},"
      "\"ams_mapping2\":[{\"ams_id\":254,\"slot_id\":0},{\"ams_id\":0,\"slot_id\":3}],"
      "\"s\":\"a\\\"b\\\\c\\u4e2dd\",\"n\":-12.5e3,\"z\":null,\"t\":true,"
      "\"ams\":{\"tray_now\":\"254\",\"ams\":[{\"tray\":[{\"id\":\"0\"}]}]}}}";
  for (const char* p = doc; *p; p++) js.feed(*p);  // 逐字节
  CHECK(!js.error());
  CHECK(js.done());
  CHECK(rec.has("print/command=project_file"));
  CHECK(rec.has("print/use_ams=false"));
  CHECK(rec.has("print/ams_mapping2/[]{"));
  CHECK(rec.has("print/ams_mapping2/[]/ams_id=254"));
  CHECK(rec.has("print/ams_mapping2/[]/slot_id=3"));
  CHECK(rec.has("print/ams_mapping2/[]}"));
  CHECK(rec.has("print/s=a\"b\\c?d"));
  CHECK(rec.has("print/n=-12.5e3"));
  CHECK(rec.has("print/z=null"));
  CHECK(rec.has("print/t=true"));
  CHECK(rec.has("print/ams/tray_now=254"));
  CHECK(rec.has("print/ams/ams/[]/tray/[]/id=0"));

  // 深层嵌套（超过记录深度）仍能正确跳过，之后的字段路径正确
  rec.items.clear();
  js.reset();
  const char* deep = "{\"a\":[[[[[[[[[[{\"k\":1}]]]]]]]]]],\"b\":{\"c\":2}}";
  js.feed(deep, std::strlen(deep));
  CHECK(!js.error() && js.done());
  CHECK(rec.has("b/c=2"));

  // 格式错误
  js.reset();
  js.feed("{\"a\":1,}", 8);
  CHECK(js.error());
  js.reset();
  js.feed("{\"a\" 1}", 7);
  CHECK(js.error());
  js.reset();
  js.feed("{\"a\":1", 6);
  CHECK(!js.error() && !js.done());  // 不完整
}

// ---------------- MQTT 报文 ----------------
struct MqttRec : semi::mqtt::Listener {
  int connack = -1, subResult = -1, pings = 0, begins = 0, ends = 0;
  uint16_t subId = 0;
  std::string topic, payload;
  size_t payloadLen = 0;
  void onConnAck(uint8_t rc) override { connack = rc; }
  void onSubAck(uint16_t id, uint8_t r) override {
    subId = id;
    subResult = r;
  }
  void onPingResp() override { pings++; }
  void onPublishBegin(const char* t, size_t len) override {
    begins++;
    topic = t;
    payloadLen = len;
    payload.clear();
  }
  void onPublishData(const uint8_t* d, size_t n) override { payload.append((const char*)d, n); }
  void onPublishEnd(uint8_t, uint16_t) override { ends++; }
};

static void test_mqtt_codec() {
  title("MQTT 报文：CONNECT 编码；CONNACK / SUBACK / PINGRESP / 长消息 PUBLISH 分段解析");
  using namespace semi::mqtt;
  uint8_t buf[600];
  size_t n = buildConnect(buf, sizeof(buf), "semi-ams-1", "bblp", "12345678", 60);
  const uint8_t expectHead[] = {0x10, 38, 0, 4, 'M', 'Q', 'T', 'T', 4, 0xC2, 0, 60, 0, 10};
  CHECK(n == 40);
  CHECK(std::memcmp(buf, expectHead, sizeof(expectHead)) == 0);
  CHECK(std::memcmp(buf + 14, "semi-ams-1", 10) == 0);
  CHECK(buf[24] == 0 && buf[25] == 4 && std::memcmp(buf + 26, "bblp", 4) == 0);

  n = buildSubscribe(buf, sizeof(buf), 1, "device/01P00A000000000/report", 0);
  CHECK(n > 0 && buf[0] == 0x82 && buf[1] == n - 2 && buf[3] == 1);

  // 长消息（剩余长度两字节）
  std::string big = "{\"print\":{\"command\":\"push_status\",\"pad\":\"";
  big += std::string(400, 'x');
  big += "\"}}";
  n = buildPublish(buf, sizeof(buf), "device/SN/report", big.data(), big.size());
  CHECK(n > 0 && buf[0] == 0x30 && (buf[1] & 0x80));

  MqttRec rec;
  Reader r(&rec);
  const uint8_t connack[] = {0x20, 2, 0, 0};
  const uint8_t suback[] = {0x90, 3, 0, 1, 0};
  const uint8_t ping[] = {0xD0, 0};
  r.feed(connack, sizeof(connack));
  r.feed(suback, sizeof(suback));
  r.feed(ping, sizeof(ping));
  CHECK(rec.connack == 0);
  CHECK(rec.subId == 1 && rec.subResult == 0);
  CHECK(rec.pings == 1);
  // 分成很多小段喂进去
  for (size_t i = 0; i < n; i += 7) r.feed(buf + i, std::min<size_t>(7, n - i));
  CHECK(!r.error());
  CHECK(rec.begins == 1 && rec.ends == 1);
  CHECK(rec.topic == "device/SN/report");
  CHECK(rec.payloadLen == big.size() && rec.payload == big);
  // 连着两条
  r.feed(buf, n);
  r.feed(ping, sizeof(ping));
  CHECK(rec.ends == 2 && rec.pings == 2);
  // 认证失败
  const uint8_t bad[] = {0x20, 2, 0, 5};
  r.feed(bad, sizeof(bad));
  CHECK(rec.connack == 5);
}

// ---------------- 打印机消息解读 ----------------
struct Printer {
  BambuMonitor b;
  uint32_t now = 1000;
  Printer() {
    b.setLogger(logger);
    b.reset();
  }
  void msg(const std::string& json) {
    b.beginMessage();
    b.feed(json.data(), json.size());
    b.endMessage(now);
  }
  bool start() { return b.takeStart(nullptr); }
  bool unload() { return b.takeUnload(nullptr); }
  void wait(uint32_t ms) {
    now += ms;
    b.tick(now);
  }
};

static std::string status(const std::string& fields) {
  return "{\"print\":{\"command\":\"push_status\",\"sequence_id\":\"1\"," + fields + "}}";
}

static void test_bambu_print_start() {
  title("打印机：project_file 回报判断是否用外置料盘");
  Printer p;
  p.msg(status("\"gcode_state\":\"IDLE\",\"hw_switch_state\":1,\"ams\":{\"tray_now\":\"255\"}"));
  CHECK(!p.start() && !p.unload());
  CHECK(std::strcmp(p.b.gcodeState(), "IDLE") == 0);
  CHECK(p.b.hwSwitch() == 1);

  // 没有 AMS：use_ams=false
  p.msg("{\"print\":{\"command\":\"project_file\",\"param\":\"Metadata/plate_1.gcode\",\"use_ams\":false,"
        "\"ams_mapping\":[],\"result\":\"success\",\"sequence_id\":\"2\"}}");
  CHECK(p.start());
  CHECK(!p.start());  // 取出后清除

  // AMS 打印：不送料
  p.msg("{\"print\":{\"command\":\"project_file\",\"use_ams\":true,\"ams_mapping\":[0,1],"
        "\"ams_mapping2\":[{\"ams_id\":0,\"slot_id\":0},{\"ams_id\":0,\"slot_id\":1}]}}");
  CHECK(!p.start());
  // AMS + 外置料盘混合
  p.msg("{\"print\":{\"command\":\"project_file\",\"use_ams\":true,\"ams_mapping\":[-1,3],"
        "\"ams_mapping2\":[{\"ams_id\":254,\"slot_id\":0},{\"ams_id\":0,\"slot_id\":3}]}}");
  CHECK(p.start());
  // 字段顺序反过来也行
  p.msg("{\"print\":{\"ams_mapping2\":[{\"slot_id\":0,\"ams_id\":255}],\"use_ams\":true,"
        "\"command\":\"project_file\"}}");
  CHECK(p.start());
  // {"ams_id":255,"slot_id":255} 是"未分配"，不算外置料盘
  p.msg("{\"print\":{\"command\":\"project_file\",\"use_ams\":true,"
        "\"ams_mapping2\":[{\"ams_id\":255,\"slot_id\":255},{\"ams_id\":0,\"slot_id\":2}]}}");
  CHECK(!p.start());
  // 命令失败
  p.msg("{\"print\":{\"command\":\"project_file\",\"use_ams\":false,\"result\":\"FAIL\",\"reason\":\"x\"}}");
  CHECK(!p.start());
  // 格式错误的消息不处理
  p.msg("{\"print\":{\"command\":\"project_file\",\"use_ams\":false");
  CHECK(!p.start());
  CHECK(p.b.badMessages() == 1);
}

static void test_bambu_state_fallback() {
  title("打印机：打印状态从空闲变成 PREPARE 且当前料盘是外置料盘时也开始送料");
  Printer p;
  p.msg(status("\"gcode_state\":\"RUNNING\",\"ams\":{\"tray_now\":\"254\"}"));
  CHECK(!p.start());  // 刚连上就已经在打印：不知道之前的状态，不触发
  p.msg(status("\"gcode_state\":\"FINISH\""));
  p.msg(status("\"gcode_state\":\"PREPARE\""));
  CHECK(p.start());
  p.msg(status("\"gcode_state\":\"RUNNING\""));
  CHECK(!p.start());  // PREPARE → RUNNING 不是新的开始
  p.msg(status("\"gcode_state\":\"IDLE\",\"ams\":{\"tray_now\":\"2\"}"));
  p.msg(status("\"gcode_state\":\"PREPARE\""));
  CHECK(!p.start());  // AMS 料盘
}

static void test_bambu_unload() {
  title("打印机退料：等打印机的耗材传感器变成无再退；等不到就超时后退");
  Printer p;
  p.b.setUnloadWaitMs(120000);
  p.msg(status("\"gcode_state\":\"FINISH\",\"hw_switch_state\":1,\"ams\":{\"tray_now\":\"254\"}"));
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"target\":255,\"curr_temp\":220,\"tar_temp\":220,"
        "\"result\":\"success\"}}");
  CHECK(!p.unload());
  CHECK(p.b.unloadPending());
  p.wait(30000);
  CHECK(!p.unload());
  p.msg(status("\"stg_cur\":22"));
  CHECK(!p.unload());
  p.msg(status("\"hw_switch_state\":0"));
  CHECK(p.unload());
  CHECK(!p.b.unloadPending());

  // 一直等不到传感器变化：超时后退
  p.msg(status("\"hw_switch_state\":1"));
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"target\":255}}");
  p.wait(119000);
  CHECK(!p.unload());
  p.wait(2000);
  CHECK(p.unload());

  // 空闲时传感器从有变无：备用判断直接退料（3 分钟内刚退过料时不重复触发）
  p.msg(status("\"hw_switch_state\":0"));
  CHECK(!p.unload());
  p.msg(status("\"hw_switch_state\":1"));
  p.wait(200000);
  p.msg(status("\"hw_switch_state\":0"));
  CHECK(p.unload());
  // 收到退料命令时打印机里已经没有耗材：马上退
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"target\":255}}");
  CHECK(p.unload());

  // 进料命令（target 254）只记录
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"target\":254}}");
  CHECK(!p.unload() && !p.start());

  // 等待时间设为 0：马上退
  Printer q;
  q.b.setUnloadWaitMs(0);
  q.msg(status("\"gcode_state\":\"IDLE\",\"hw_switch_state\":1"));
  q.msg("{\"print\":{\"command\":\"unload_filament\"}}");
  CHECK(q.unload());
}

static void test_bambu_hw_switch_fallback() {
  title("打印机：空闲时耗材传感器从有变无（屏幕上退料）也退料；打印中断料不退");
  Printer p;
  p.msg(status("\"gcode_state\":\"RUNNING\",\"hw_switch_state\":1"));
  p.msg(status("\"hw_switch_state\":0"));
  CHECK(!p.unload());  // 打印中断料
  p.msg(status("\"hw_switch_state\":1"));
  p.msg(status("\"gcode_state\":\"FINISH\""));
  p.msg(status("\"hw_switch_state\":0"));
  CHECK(p.unload());
  // 刚连上、还不知道打印状态：不触发
  Printer q;
  q.msg(status("\"hw_switch_state\":1"));
  q.msg(status("\"hw_switch_state\":0"));
  CHECK(!q.unload());
}

static void test_fsm_printer_commands() {
  title("状态机：打印机的开始/退料命令不会来回切换");
  Commands st;
  st.printerStart = true;
  Commands ul;
  ul.printerUnload = true;

  Sim s(Inputs{true, true, false, false});  // 耗材就绪
  s.run(10, st);
  CHECK(s.st() == State::Printing);
  s.run(10, st);  // 重复收到
  CHECK(s.st() == State::Printing);
  s.run(10, ul);
  CHECK(s.st() == State::Unloading);
  s.run(10, ul);
  CHECK(s.st() == State::Unloading);
  s.run(10, st);  // 退料中又开始打印：回到打印中
  CHECK(s.st() == State::Printing);

  Sim r(Inputs{true, false, false, false});  // 已退料
  CHECK(r.st() == State::Retracted);
  r.run(10, st);
  CHECK(r.st() == State::Loading);
  r.in.ready = true;
  r.run(20);
  CHECK(r.st() == State::Printing);

  Sim n;  // 无耗材
  n.run(10, st);
  CHECK(n.st() == State::NoMaterial);
  CHECK(std::strcmp(n.fsm.note(), "打印机已开始打印，但没有耗材") == 0);
  n.run(10, ul);
  CHECK(n.st() == State::NoMaterial);

  Sim e(Inputs{true, true, false, false});
  e.press(false, true);
  CHECK(e.st() == State::Emergency);
  e.run(10, st);
  e.run(10, ul);
  CHECK(e.st() == State::Emergency);

  Sim k(Inputs{true, true, false, false});
  k.run(10, ul);  // 就绪时收到退料：不动
  CHECK(k.st() == State::Ready);
}

static std::string extruder(int info, int snow) {
  return "\"device\":{\"extruder\":{\"info\":[{\"id\":0,\"info\":" + std::to_string(info) + ",\"snow\":" +
         std::to_string(snow) + ",\"spre\":0,\"star\":0}],\"state\":2}}";
}

static void test_bambu_new_protocol() {
  title("新协议打印机（P2S / H2）：用 extruder.info 判断外置料盘和挤出机耗材，tray_now / hw_switch_state 不再作数");
  Printer p;
  // 外置料盘：snow = 0xFF00（ams 255、slot 0）；info bit1 = 有耗材；tray_now 是 "0"（新协议下不可靠）
  p.msg(status("\"gcode_state\":\"IDLE\",\"hw_switch_state\":3,\"ams\":{\"tray_now\":\"0\"}," + extruder(78, 65280)));
  CHECK(p.b.externalActive() == 1);
  CHECK(p.b.hwSwitch() == 1);
  p.msg(status("\"gcode_state\":\"SLICING\""));
  CHECK(!p.start());
  p.msg(status("\"gcode_state\":\"PREPARE\""));
  CHECK(p.start());  // IDLE → SLICING → PREPARE 也算开始
  p.msg(status("\"gcode_state\":\"FINISH\""));
  // hw_switch_state 变化不影响（以 extruder.info 为准）
  p.msg(status("\"hw_switch_state\":2"));
  CHECK(!p.unload());
  CHECK(p.b.hwSwitch() == 1);
  // Studio 2.x 退料：target 255、slot_id 255、ams_id 255
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"ams_id\":255,\"slot_id\":255,\"target\":255,"
        "\"curr_temp\":220,\"tar_temp\":220,\"result\":\"success\"}}");
  CHECK(!p.unload());
  CHECK(p.b.unloadPending());
  p.msg(status(extruder(76, 65535)));  // info bit1 = 0：挤出机里没耗材了
  CHECK(p.b.hwSwitch() == 0);
  CHECK(p.unload());

  // AMS 料盘：snow = 3（ams 0、slot 3）
  Printer q;
  q.msg(status("\"gcode_state\":\"IDLE\",\"ams\":{\"tray_now\":\"254\"}," + extruder(78, 3)));
  CHECK(q.b.externalActive() == 0);  // 新协议下不看 tray_now
  q.msg(status("\"gcode_state\":\"PREPARE\""));
  CHECK(!q.start());
  // 0xFEFF（ams 254、slot 255）是"没有"，不算外置料盘
  q.msg(status(extruder(78, 65279)));
  CHECK(q.b.externalActive() == 0);
}

static void test_bambu_change_filament_variants() {
  title("ams_change_filament：区分退料、进外置料盘、AMS 退料");
  Printer p;
  p.b.setUnloadWaitMs(0);
  p.msg(status("\"gcode_state\":\"FINISH\",\"hw_switch_state\":1"));
  // 新协议从 Studio 进外置料盘：target 255 但 slot_id 0
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"ams_id\":255,\"slot_id\":0,\"target\":255}}");
  CHECK(!p.unload());
  // AMS 退料
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"ams_id\":0,\"slot_id\":255,\"target\":255}}");
  CHECK(!p.unload());
  // 新协议外置料盘退料（ams_id 是字符串也行）
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"ams_id\":\"255\",\"slot_id\":\"255\",\"target\":255}}");
  CHECK(p.unload());
  // 老协议：只有 target
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"target\":255}}");
  CHECK(p.unload());
  // 老协议 hw_switch_state 是位掩码：bit0 才是挤出机
  Printer q;
  q.msg(status("\"gcode_state\":\"IDLE\",\"hw_switch_state\":3"));
  CHECK(q.b.hwSwitch() == 1);
  q.msg(status("\"hw_switch_state\":2"));
  CHECK(q.b.hwSwitch() == 0);
  CHECK(q.unload());  // 空闲时 1 → 0
}

static void test_mqtt_reader_edge() {
  title("MQTT 解析：回调里断开后不再处理剩余数据；空主题；非法 QoS");
  using namespace semi::mqtt;
  struct StopRec : MqttRec {
    Reader* r = nullptr;
    void onConnAck(uint8_t rc) override {
      MqttRec::onConnAck(rc);
      r->stop();
    }
  } rec;
  Reader r(&rec);
  rec.r = &r;
  uint8_t buf[64];
  const uint8_t connack[] = {0x20, 2, 0, 5};
  size_t n = 0;
  std::memcpy(buf, connack, 4);
  n = 4 + buildPublish(buf + 4, sizeof(buf) - 4, "t", "{}", 2);
  r.feed(buf, n);
  CHECK(rec.connack == 5);
  CHECK(rec.begins == 0);  // 同一段数据里后面的 PUBLISH 没有被处理
  r.reset();
  MqttRec plain;
  r.setListener(&plain);
  const uint8_t pub1[] = {0x30, 5, 0, 1, 'a', '{', '}'};
  const uint8_t pub0[] = {0x30, 4, 0, 0, '{', '}'};
  r.feed(pub1, sizeof(pub1));
  r.feed(pub0, sizeof(pub0));
  CHECK(plain.ends == 2 && plain.topic.empty() && plain.payload == "{}");
  const uint8_t qos3[] = {0x36, 4, 0, 0, '{', '}'};
  r.feed(qos3, sizeof(qos3));
  CHECK(r.error());
}

static void test_bambu_pull_out_prompt() {
  title("外置料盘退料：打印机提示拔出耗材（0x07FFC003）就马上退；之后传感器变化不重复触发");
  Printer p;
  p.b.setUnloadWaitMs(120000);
  p.msg(status("\"gcode_state\":\"RUNNING\",\"print_error\":0," + extruder(78, 65280)));
  p.msg(status("\"gcode_state\":\"FAILED\",\"print_error\":50348044"));  // 0x0300400C 打印任务已取消
  CHECK(!p.unload());
  p.msg("{\"print\":{\"command\":\"ams_change_filament\",\"ams_id\":255,\"slot_id\":255,\"target\":255}}");
  CHECK(!p.unload() && p.b.unloadPending());
  p.wait(44000);
  p.msg(status("\"print_error\":134201347"));  // 0x07FFC003 请从挤出机拔出外置料盘的耗材
  CHECK(p.unload());
  CHECK(!p.b.unloadPending());
  p.wait(2000);
  p.msg(status(extruder(76, 65535)));  // 进料器把耗材拉出来了
  CHECK(!p.unload());                  // 不重复触发
  // 没有收到退料命令也认这个提示（例如在屏幕上退料）
  Printer q;
  q.msg(status("\"gcode_state\":\"FINISH\",\"print_error\":0"));
  q.msg(status("\"print_error\":134201347"));
  CHECK(q.unload());
}

static void test_fsm_both_halls_warning() {
  title("打印中两颗霍尔同时有效：不送料，并在备注里提示");
  Sim s(Inputs{true, true, true, false});
  s.press(true);
  s.run(50);
  CHECK(s.m() == MotorDir::Forward);
  s.in.full = true;  // EMPTY 和 FULL 同时有效
  s.run(20);
  CHECK(s.m() == MotorDir::Stop);
  CHECK(std::strcmp(s.fsm.note(), "EMPTY和FULL同时有效，不送料") == 0);
  s.in.lack = false;
  s.run(20);
  CHECK(std::strcmp(s.fsm.note(), "") == 0);
}

