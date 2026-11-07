#include "net.h"

#include <ArduinoOTA.h>
#include <ESP8266HTTPUpdateServer.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <Updater.h>
#include <WiFiManager.h>

#include "app.h"
#include "crash.h"
#include "cloud.h"
#include "config.h"
#include "lanmqtt.h"
#include "netinfo.h"
#include "printer.h"
#include "settings.h"

namespace {

WiFiManager wm;
WiFiManagerParameter* gKeyParam = nullptr;
ESP8266WebServer server(80);
ESP8266HTTPUpdateServer updater;

enum class Phase : uint8_t { Portal, Offline, Online };
Phase gPhase = Phase::Offline;
bool gCloudTried = false;
bool gServicesUp = false;
bool gOwnMdns = false;  // 没启用点灯时由本固件提供 semi-ams.local（点灯启用后 mDNS 由点灯库接管）
bool gWasConnected = false;
uint32_t gLastRetry = 0;
uint32_t gOfflineSince = 0;
const char* apPass() { return AP_PASSWORD[0] ? AP_PASSWORD : nullptr; }

void goOffline(const char* why) {
  gPhase = Phase::Offline;
  gOfflineSince = millis();
  gLastRetry = millis();
  WiFi.mode(WIFI_STA);
  WiFi.begin();  // 用已保存的 WiFi 后台重连
  LOG(SEMI_PS, why);  // why 在 Flash 里
}

// ---------------- 网页 ----------------
const char PAGE_HEAD[] PROGMEM = R"HTML(<!doctype html><html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1"><title>SEMI-AMS</title><style>
body{font-family:-apple-system,system-ui,sans-serif;margin:0;background:#f4f5f7;color:#222}
main{max-width:560px;margin:0 auto;padding:16px}h1{font-size:20px;margin:8px 0 16px}h1 small{font-size:13px;color:#888}
.card{background:#fff;border-radius:10px;padding:14px 16px;margin-bottom:14px;box-shadow:0 1px 3px #0001}
.card h2{font-size:15px;margin:0 0 10px;color:#555}
.grid{display:grid;grid-template-columns:auto 1fr;gap:6px 14px;font-size:14px}.v{font-weight:600}
.btns{display:flex;flex-wrap:wrap;gap:8px}
button{border:0;border-radius:8px;padding:10px 14px;font-size:14px;background:#1677ff;color:#fff}
button.red{background:#e5484d}button.gray{background:#6b7280}button.green{background:#16a34a}
label{display:flex;justify-content:space-between;align-items:center;font-size:14px;padding:5px 0;gap:10px}
input{width:110px;padding:6px;border:1px solid #ccc;border-radius:6px;font-size:14px}input.wide{width:180px}
pre{font-size:12px;background:#111;color:#ddd;padding:10px;border-radius:8px;white-space:pre-wrap;max-height:280px;overflow-y:auto}
.dot{display:inline-block;width:9px;height:9px;border-radius:50%;background:#bbb;margin-right:6px}.on{background:#16a34a}
@media (prefers-color-scheme:dark){body{background:#111;color:#eee}.card{background:#1c1c1e;box-shadow:none}
.card h2{color:#aaa}input{background:#222;color:#eee;border-color:#444}}
</style></head><body><main>
<h1>SEMI-AMS <small id="ver"></small></h1>
<div class="card"><h2>状态</h2><div class="grid">
<span>工作状态</span><span class="v" id="state">-</span>
<span>电机</span><span class="v" id="motor">-</span>
<span>进料口开关</span><span id="in"></span>
<span>就绪开关</span><span id="ready"></span>
<span>EMPTY 霍尔</span><span id="lack"></span>
<span>FULL 霍尔</span><span id="full"></span>
<span>黑色按钮</span><span id="bs"></span>
<span>红色按钮</span><span id="be"></span>
<span>定时任务</span><span id="tk"></span>
<span>运行时长</span><span id="up">-</span>
<span>空闲内存</span><span id="hp">-</span>
<span>WiFi</span><span id="wifi">-</span>
<span>MAC</span><span id="mac">-</span>
<span>点灯</span><span id="cloud">-</span>
</div></div>
<div class="card"><h2>操作</h2><div class="btns">
<button class="green" onclick="cmd('start')">开始/结束打印</button>
<button class="red" onclick="cmd('emg')">紧急停止/解除</button>
<button onclick="cmd('jogf')">正转 2 秒</button>
<button onclick="cmd('jogr')">反转 2 秒</button>
<button class="gray" onclick="if(confirm('确定重启？'))cmd('restart')">重启</button>
</div><p id="msg" style="font-size:13px;color:#888;margin:8px 0 0">点动只在"无耗材 / 耗材就绪 / 已退料"状态下可用，用来确认电机方向和速度</p></div>
)HTML";

const char PARAM_HEAD[] PROGMEM = R"HTML(<div class="card"><h2>参数</h2><form method="post" action="/api/settings">
)HTML";

const char MQTT_HEAD[] PROGMEM = R"HTML(<div class="card"><h2>拓竹打印机（MQTT）</h2><div class="grid">
<span>连接</span><span class="v" id="mq">-</span>
<span>打印机</span><span id="mp">-</span>
<span>最近事件</span><span id="me">-</span>
</div><form method="post" action="/api/mqtt" style="margin-top:10px">
)HTML";

const char MQTT_TAIL[] PROGMEM = R"HTML(<div class="btns" style="margin-top:10px"><button type="submit">保存</button></div></form>
<p style="font-size:13px;color:#888;margin:8px 0 0">打印机用外置料盘开始打印时自动送料（响应进料）；在打印机上退料时自动退料（响应退料）。
几台 SEMI-AMS 接同一台打印机时，这次不用的那几台取消"响应进料"，否则会一起送料；退料只有正在打印中的那台会执行。
打印机和本设备要在同一个局域网，打印机可以保持连接拓竹云。IP、序列号、访问码在打印机屏幕的设置里能看到（局域网 / 设备信息）。</p></div>
)HTML";

// 局域网 MQTT：把本机状态发布给局域网里的服务器（Home Assistant / Node-RED / 群控程序等）
const char LANMQTT_HEAD[] PROGMEM = R"HTML(<div class="card"><h2>局域网 MQTT</h2><div class="grid">
<span>连接</span><span class="v" id="lq">-</span>
</div><form method="post" action="/api/lanmqtt" style="margin-top:10px">
)HTML";

const char LANMQTT_TAIL[] PROGMEM = R"HTML(<div class="btns" style="margin-top:10px"><button type="submit">保存</button></div></form>
<p style="font-size:13px;color:#888;margin:8px 0 0">把状态发布到局域网 MQTT 服务器：<code>前缀 + 设备ID/state</code> 是状态，<code>前缀 + 设备ID/availability</code> 是在线情况（设备ID 是 MAC 去掉冒号）。
几台设备订阅 <code>前缀 + +/state</code> 就能一起收到。开了"允许远程控制"才接受 <code>前缀 + control</code> 上的命令（开始/结束、急停、点动、重启），局域网里能连上这个服务器的程序都能发，按需要开。
密码不显示，留空表示不修改；用户名清空就不登录。</p></div>
)HTML";

const char PAGE_TAIL[] PROGMEM = R"HTML(<div class="btns" style="margin-top:10px"><button type="submit">保存</button></div></form></div>
<div class="card"><h2>日志</h2><pre id="log"></pre><div class="btns">
<a href="/api/log" target="_blank"><button class="gray">查看完整日志（重启后也保留）</button></a>
<button class="gray" onclick="if(confirm('清空保存的日志文件？'))fetch('/api/log/clear',{method:'POST'})">清空日志文件</button>
</div></div>
<div class="card"><h2>维护</h2><div class="btns">
<a href="/update"><button class="gray">固件升级</button></a>
<button class="red" onclick="if(confirm('清除 WiFi 和点灯密钥并重启？重启后连接 SEMI-AMS 热点重新配网'))fetch('/api/reset',{method:'POST'})">重置配网</button>
</div></div></main><script>
function dot(v){return '<span class="dot'+(v?' on':'')+'"></span>'+(v?'有效':'无')}
function btn(p,pin,lv){return '<span class="dot'+(p?' on':'')+'"></span>'+(p?'按下':'松开')+' <span style="color:#888">（'+pin+' '+(lv?'高':'低')+'）</span>'}
function cmd(c){fetch('/api/cmd?c='+c,{method:'POST'}).then(r=>r.json()).then(j=>{document.getElementById('msg').textContent=j.msg})}
function upd(){fetch('/api/status').then(r=>r.json()).then(s=>{
for(const k of ['state','motor','up','wifi','cloud','ver','mq','mp','me','lq'])document.getElementById(k).textContent=s[k];
document.getElementById('mac').textContent=s.mac||'-';
for(const k of ['in','ready','lack','full'])document.getElementById(k).innerHTML=dot(s[k]);
document.getElementById('bs').innerHTML=btn(s.bs,'GPIO0',s.g0);
document.getElementById('be').innerHTML=btn(s.be,'GPIO15',s.g15);
document.getElementById('tk').textContent='最大间隔 '+s.tg+'ms，被推迟 '+s.tl+' 次';
if(s.hp){var h=s.hp.split('/');document.getElementById('hp').textContent=h[0]+'（开机后最低 '+h[1]+'，最大连续 '+h[2]+'）';}
const l=document.getElementById('log');l.textContent=s.log.join('\n');l.scrollTop=l.scrollHeight;}).catch(()=>{})}
upd();setInterval(upd,1500);
</script></body></html>)HTML";

String jsonEscape(const String& s) {
  String o;
  o.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') {
      o += '\\';
      o += c;
    } else if ((uint8_t)c < 0x20) {
      o += ' ';
    } else {
      o += c;
    }
  }
  return o;
}

void handleRoot() {
  String form;
  size_t n;
  const SettingDef* defs = settingDefs(n);
  for (size_t i = 0; i < n; i++) {
    form += F("<label>");
    form += FPSTR(defs[i].label);  // 中文名在 Flash 里
    form += F("（");
    form += defs[i].unit;
    form += F("）<input type=\"number\" name=\"");
    form += defs[i].key;
    form += F("\" min=\"");
    form += defs[i].min;
    form += F("\" max=\"");
    form += defs[i].max;
    form += F("\" value=\"");
    form += settingsGet(defs[i].key);
    form += F("\"></label>\n");
  }
  form += F("<label>点灯密钥（重启生效）<input class=\"wide\" name=\"blinker_key\" pattern=\"[a-zA-Z0-9]*\" placeholder=\"");
  form += gSettings.blinkerKey[0] ? F("已设置，留空不修改") : F("未设置");
  form += F("\"></label>\n");

  String mq;
  mq += F("<label>启用<input type=\"checkbox\" name=\"enable\" value=\"1\" style=\"width:auto\"");
  if (gSettings.mqttEnable) mq += F(" checked");
  mq += F("></label><input type=\"hidden\" name=\"has_enable\" value=\"1\">\n");
  mq += F("<label>响应进料<input type=\"checkbox\" name=\"load\" value=\"1\" style=\"width:auto\"");
  if (gSettings.mqttAutoLoad) mq += F(" checked");
  mq += F("></label>\n<label>响应退料<input type=\"checkbox\" name=\"unload\" value=\"1\" style=\"width:auto\"");
  if (gSettings.mqttAutoUnload) mq += F(" checked");
  mq += F("></label><input type=\"hidden\" name=\"has_auto\" value=\"1\">\n");
  mq += F("<label>打印机 IP<input class=\"wide\" name=\"ip\" placeholder=\"192.168.1.50\" value=\"");
  mq += gSettings.mqttIp;
  mq += F("\"></label>\n<label>序列号<input class=\"wide\" name=\"sn\" pattern=\"[A-Za-z0-9]{8,20}\" value=\"");
  mq += gSettings.mqttSn;
  mq += F("\"></label>\n<label>访问码（配对码）<input class=\"wide\" name=\"code\" type=\"password\" "
          "pattern=\"[A-Za-z0-9]{6,16}\" placeholder=\"");
  mq += gSettings.mqttCode[0] ? F("已设置，留空不修改") : F("8 位");
  mq += F("\"></label>\n<label>打印机退料后最多等（秒）<input type=\"number\" name=\"wait\" min=\"0\" max=\"600\" value=\"");
  mq += gSettings.mqttUnloadWaitS;
  mq += F("\"></label>\n");

  String lan;
  lan += F("<label>启用<input type=\"checkbox\" name=\"enable\" value=\"1\" style=\"width:auto\"");
  if (gSettings.lanMqttEnable) lan += F(" checked");
  lan += F("></label><input type=\"hidden\" name=\"has_enable\" value=\"1\">\n");
  lan += F("<label>允许远程控制<input type=\"checkbox\" name=\"control\" value=\"1\" style=\"width:auto\"");
  if (gSettings.lanMqttControl) lan += F(" checked");
  lan += F("></label>\n");
  lan += F("<label>IP<input class=\"wide\" name=\"ip\" placeholder=\"192.168.5.10\" value=\"");
  lan += gSettings.lanMqttIp;
  lan += F("\"></label>\n<label>端口<input type=\"number\" name=\"port\" min=\"1\" max=\"65535\" value=\"");
  lan += (unsigned int)gSettings.lanMqttPort;
  lan += F("\"></label>\n<label>topic 前缀<input class=\"wide\" name=\"topic\" placeholder=\"semiams/\" value=\"");
  lan += gSettings.lanMqttTopic;
  lan += F("\"></label>\n<label>用户名<input class=\"wide\" name=\"user\" autocomplete=\"off\" value=\"");
  lan += gSettings.lanMqttUser;
  lan += F("\"></label>\n<label>密码<input class=\"wide\" type=\"password\" name=\"pass\" autocomplete=\"new-password\" placeholder=\"");
  lan += gSettings.lanMqttPass[0] ? F("已设置") : F("不需要就留空");
  lan += F("\"></label>\n<p style=\"font-size:13px;color:#888;margin:4px 0 0\">本机状态 topic：<code>");
  lan += lanmqtt::topicText();
  lan += F("</code></p>\n");

  server.chunkedResponseModeStart(200, "text/html; charset=utf-8");
  server.sendContent_P(PAGE_HEAD);
  server.sendContent_P(MQTT_HEAD);
  server.sendContent(mq);
  server.sendContent_P(MQTT_TAIL);
  server.sendContent_P(LANMQTT_HEAD);
  server.sendContent(lan);
  server.sendContent_P(LANMQTT_TAIL);
  server.sendContent_P(PARAM_HEAD);
  server.sendContent(form);
  server.sendContent_P(PAGE_TAIL);
  server.chunkedResponseFinalize();
}

void handleStatus() {
  app::Status s = app::status();
  String j;
  j.reserve(512);
  j += F("{\"ver\":\"v" FW_VERSION "\",\"state\":\"");
  j += jsonEscape(app::stateText());
  j += F("\",\"motor\":\"");
  j += FPSTR(semi::Fsm::motorName(s.motor));
  if (s.motor != semi::MotorDir::Stop) {
    j += ' ';
    j += s.duty;
    j += '%';
  }
  j += F("\",\"in\":");
  j += s.in ? 1 : 0;
  j += F(",\"ready\":");
  j += s.ready ? 1 : 0;
  j += F(",\"lack\":");
  j += s.lack ? 1 : 0;
  j += F(",\"full\":");
  j += s.full ? 1 : 0;
  j += F(",\"bs\":");
  j += s.btnStart ? 1 : 0;
  j += F(",\"be\":");
  j += s.btnEmg ? 1 : 0;
  j += F(",\"g0\":");
  j += s.rawGpio0 ? 1 : 0;
  j += F(",\"g15\":");
  j += s.rawGpio15 ? 1 : 0;
  j += F(",\"tg\":");
  j += s.tickGapMs;
  j += F(",\"tl\":");
  j += s.tickLate;
  j += F(",\"hp\":\"");  // 空闲内存/开机后最低/最大连续
  j += ESP.getFreeHeap();
  j += '/';
  j += crash::heapMin();
  j += '/';
  j += ESP.getMaxFreeBlockSize();
  j += '"';
  j += F(",\"up\":\"");
  j += app::uptimeText();
  j += F("\",\"wifi\":\"");
  j += jsonEscape(WiFi.SSID());
  j += ' ';
  j += WiFi.RSSI();
  j += F("dBm ");
  j += WiFi.localIP().toString();
  char ipBuf[16], macBuf[18];
  netinfo::ip(ipBuf, sizeof(ipBuf));
  netinfo::mac(macBuf, sizeof(macBuf));
  j += F("\",\"ip\":\"");
  j += ipBuf;
  j += F("\",\"mac\":\"");
  j += macBuf;
  j += F("\",\"cloud\":\"");
  j += FPSTR(cloud::statusText());
  j += F("\",\"mq\":\"");
  j += jsonEscape(printer::statusText());
  j += F("\",\"mp\":\"");
  j += jsonEscape(printer::printerText());
  j += F("\",\"me\":\"");
  j += jsonEscape(printer::lastEvent());
  j += F("\",\"lq\":\"");
  j += jsonEscape(lanmqtt::statusText());
  j += F("\",\"log\":[");
  // 分块发送，避免每 1.5 秒拼一个几 KB 的大字符串造成堆碎片
  server.chunkedResponseModeStart(200, "application/json; charset=utf-8");
  server.sendContent(j);
  const size_t n = app::logCount();
  const size_t first = 0;  // 内存里的日志全部显示（最多 16 条），更早的看完整日志
  for (size_t i = first; i < n; i++) {
    j = i == first ? "\"" : ",\"";
    j += jsonEscape(app::logLine(i));
    j += '"';
    server.sendContent(j);
  }
  server.sendContent(String("]}"));
  server.chunkedResponseFinalize();
}

void sendMsg(bool ok, const String& msg) {
  String j = F("{\"ok\":");
  j += ok ? F("true") : F("false");
  j += F(",\"msg\":\"");
  j += jsonEscape(msg);
  j += F("\"}");
  server.send(ok ? 200 : 400, "application/json; charset=utf-8", j);
}

void handleCmd() {
  String c = server.arg("c");
  if (c == "start") {
    app::requestStartStop(PSTR("网页"));
    sendMsg(true, F("已发送：开始/结束"));
  } else if (c == "emg") {
    app::requestEmergency(PSTR("网页"));
    sendMsg(true, F("已发送：紧急停止/解除"));
  } else if (c == "jogf" || c == "jogr") {
    bool ok = app::requestJog(c == "jogf" ? semi::MotorDir::Forward : semi::MotorDir::Reverse,
                              gSettings.p.feedSpeed, 2000);
    sendMsg(ok, ok ? F("点动 2 秒（补料速度）") : F("只有在 无耗材/耗材就绪/已退料 状态下才能点动"));
  } else if (c == "restart") {
    sendMsg(true, F("正在重启..."));
    app::requestRestart(PSTR("网页"));
  } else {
    sendMsg(false, F("未知命令"));
  }
}

void handleSettings() {
  size_t n;
  const SettingDef* defs = settingDefs(n);
  int bad = 0;
  for (size_t i = 0; i < n; i++) {
    String v = server.arg(defs[i].key);
    v.trim();
    if (!v.length()) continue;  // 留空的输入框不修改
    if (!settingsSet(defs[i].key, v.toInt())) bad++;
  }
  String key = server.arg("blinker_key");
  key.trim();
  bool keyChanged = key.length() && key != gSettings.blinkerKey;
  if (keyChanged) settingsSetBlinkerKey(key.c_str());
  settingsSave();
  app::applySettings();
  LOG("网页：参数已保存" SEMI_PS SEMI_PS, bad ? PSTR("（部分超出范围未修改）") : "",
      keyChanged ? PSTR("，点灯密钥重启后生效") : "");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleMqtt() {
  bool bad = false;
  String ip = server.arg("ip"), sn = server.arg("sn"), code = server.arg("code"), wait = server.arg("wait");
  ip.trim();
  sn.trim();
  code.trim();
  wait.trim();
  if (!settingsSetMqttIp(ip.c_str())) bad = true;
  if (!settingsSetMqttSn(sn.c_str())) bad = true;
  if (code.length() && !settingsSetMqttCode(code.c_str())) bad = true;  // 留空不修改
  if (wait.length() && !settingsSetMqttUnloadWait(wait.toInt())) bad = true;
  if (server.hasArg("has_enable")) gSettings.mqttEnable = server.arg("enable") == "1";
  if (server.hasArg("has_auto")) {  // 复选框不勾选时不会提交，所以用 has_auto 判断表单里有这两项
    gSettings.mqttAutoLoad = server.arg("load") == "1";
    gSettings.mqttAutoUnload = server.arg("unload") == "1";
  }
  settingsSave();
  printer::applySettings();
  LOG("网页：拓竹打印机设置已保存（" SEMI_PS "，进料" SEMI_PS "，退料" SEMI_PS "）" SEMI_PS,
      gSettings.mqttEnable ? PSTR("已启用") : PSTR("未启用"), gSettings.mqttAutoLoad ? PSTR("响应") : PSTR("不响应"),
      gSettings.mqttAutoUnload ? PSTR("响应") : PSTR("不响应"), bad ? PSTR("，有格式不对的项未修改") : "");
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLanMqtt() {
  bool bad = false;
  String ip = server.arg("ip"), port = server.arg("port"), topic = server.arg("topic");
  ip.trim();
  port.trim();
  topic.trim();
  if (!settingsSetLanMqttIp(ip.c_str())) bad = true;
  if (port.length() && !settingsSetLanMqttPort(port.toInt())) bad = true;
  if (!settingsSetLanMqttTopic(topic.c_str())) bad = true;
  String user = server.arg("user"), pass = server.arg("pass");
  user.trim();
  if (!settingsSetLanMqttUser(user.c_str())) bad = true;
  if (pass.length() && !settingsSetLanMqttPass(pass.c_str())) bad = true;  // 留空 = 不修改
  if (server.hasArg("has_enable")) {
    gSettings.lanMqttEnable = server.arg("enable") == "1";
    gSettings.lanMqttControl = server.arg("control") == "1";
  }
  settingsSave();
  lanmqtt::applySettings();  // 断开旧连接，按新设置重连
  LOG("网页：局域网 MQTT 设置已保存（" SEMI_PS "，%s:%u，前缀 %s，" SEMI_PS "，" SEMI_PS "）" SEMI_PS,
      gSettings.lanMqttEnable ? PSTR("已启用") : PSTR("未启用"), gSettings.lanMqttIp,
      (unsigned)gSettings.lanMqttPort, gSettings.lanMqttTopic,
      gSettings.lanMqttUser[0] ? PSTR("登录") : PSTR("不登录"),
      gSettings.lanMqttControl ? PSTR("允许远程控制") : PSTR("不接受远程控制"),
      bad ? PSTR("，有格式不对的项未修改") : PSTR(""));
  server.sendHeader("Location", "/");
  server.send(303);
}

void handleLog() {
  server.chunkedResponseModeStart(200, "text/plain; charset=utf-8");
  static const char* const kFiles[] = {app::kLogFileOld, app::kLogFile};
  char buf[512];
  for (const char* path : kFiles) {
    File f = LittleFS.open(path, "r");
    if (!f) continue;
    while (f.available()) {
      const size_t n = f.read((uint8_t*)buf, sizeof(buf));
      if (!n) break;
      server.sendContent(buf, n);
    }
    f.close();
  }
  server.chunkedResponseFinalize();
}

void handleLogClear() {
  LittleFS.remove(app::kLogFileOld);
  LittleFS.remove(app::kLogFile);
  LOG("日志文件已清空");
  sendMsg(true, F("已清空"));
}

void handleReset() {
  server.send_P(200, PSTR("text/plain; charset=utf-8"), PSTR("已清除，正在重启"));
  net::resetAll();
}

void onSaveParams() {
  if (!gKeyParam) return;
  String k = gKeyParam->getValue();
  k.trim();
  settingsSetBlinkerKey(k.c_str());
  settingsSave();
  LOG("配网页面：点灯密钥已" SEMI_PS, k.length() ? PSTR("保存") : PSTR("清空"));
}

void startServices() {
  // 点灯库连上服务器后会用它自己的设备名调用 MDNS.begin()，
  // 所以启用点灯时本固件不注册 semi-ams.local，网页和 OTA 请直接用 IP
  gOwnMdns = !gSettings.blinkerKey[0];

  // OTA（PlatformIO：pio run -e ota -t upload）
  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { LOG("OTA 升级开始，电机已停止"); });
  ArduinoOTA.onError([](ota_error_t e) {
    LOG("OTA 失败：%u %s", (unsigned)e, Update.getErrorString().c_str());
  });
  ArduinoOTA.begin(gOwnMdns);

  server.on("/", HTTP_GET, handleRoot);
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/cmd", HTTP_POST, handleCmd);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.on("/api/mqtt", HTTP_POST, handleMqtt);
  server.on("/api/lanmqtt", HTTP_POST, handleLanMqtt);
  server.on("/api/log", HTTP_GET, handleLog);
  server.on("/api/log/clear", HTTP_POST, handleLogClear);
  server.on("/api/reset", HTTP_POST, handleReset);
  updater.setup(&server, "/update", "admin", OTA_PASSWORD);
  server.onNotFound([]() { server.send(404, "text/plain", "Not found"); });
  server.begin();
  if (gOwnMdns) MDNS.addService("http", "tcp", 80);

  gServicesUp = true;
  if (gOwnMdns)
    LOG("设备网页：http://%s/ 或 http://%s.local/", WiFi.localIP().toString().c_str(), HOSTNAME);
  else
    LOG("设备网页：http://%s/", WiFi.localIP().toString().c_str());
}

}  // namespace

namespace net {

void begin() {
  WiFi.persistent(true);
  WiFi.setAutoReconnect(true);
  WiFi.mode(WIFI_STA);

  const bool saved = wm.getWiFiIsSaved();
  wm.setDebugOutput(false);
  wm.setTitle(FW_NAME);
  wm.setHostname(HOSTNAME);
  wm.setConfigPortalBlocking(false);  // 配网时状态机照常工作
  wm.setConnectTimeout(WIFI_CONNECT_TIMEOUT_S);
  // 从没配过网：热点一直开着；配过网但暂时连不上（例如停电后路由器还没起来）：
  // 先不开热点，后台重连 OFFLINE_PORTAL_AFTER_S 秒，仍连不上再开热点 PORTAL_TIMEOUT_S 秒
  wm.setConfigPortalTimeout(saved ? PORTAL_TIMEOUT_S : 0);
  wm.setEnableConfigPortal(!saved);
  static std::vector<const char*> menu = {"wifi", "wifinoscan", "info", "restart"};
  wm.setMenu(menu);
  gKeyParam = new WiFiManagerParameter("blinker_key", "Blinker Secret（点灯密钥，不用 App 可留空）",
                                       gSettings.blinkerKey, 32, "pattern='[a-zA-Z0-9]*'");
  wm.addParameter(gKeyParam);
  wm.setSaveParamsCallback(onSaveParams);

  app::setNetLed(app::NetLed::Slow);
  if (saved) LOG("连接已保存的 WiFi...");
  else LOG("尚未配网");
  if (wm.autoConnect(AP_NAME, apPass())) {
    gPhase = Phase::Online;
  } else if (saved) {
    goOffline(PSTR("暂时连不上 WiFi，后台重连中（3 分钟后仍连不上会打开 SEMI-AMS 配网热点）"));
  } else {
    gPhase = Phase::Portal;
    LOG("配网模式：手机连接热点 %s，在弹出的页面（或 192.168.4.1）选择 WiFi 并填写点灯密钥", AP_NAME);
  }
}

void loop() {
  const bool conn = WiFi.status() == WL_CONNECTED;

  switch (gPhase) {
    case Phase::Portal:
      wm.process();
      if (!wm.getConfigPortalActive()) {
        if (WiFi.status() == WL_CONNECTED) gPhase = Phase::Online;
        else goOffline(PSTR("配网热点已关闭，后台继续尝试连接已保存的 WiFi"));
      }
      break;

    case Phase::Offline:
      if (conn) {
        gPhase = Phase::Online;
      } else if (millis() - gOfflineSince > OFFLINE_PORTAL_AFTER_S * 1000UL) {
        gPhase = Phase::Portal;
        wm.setEnableConfigPortal(true);
        wm.startConfigPortal(AP_NAME, apPass());  // 非阻塞，PORTAL_TIMEOUT_S 后自动关闭
        LOG("一直连不上 WiFi，打开配网热点 %s（%d 分钟）", AP_NAME, PORTAL_TIMEOUT_S / 60);
      } else if (millis() - gLastRetry > 60000) {
        gLastRetry = millis();
        WiFi.begin();
      }
      break;

    case Phase::Online:
      if (!gCloudTried) {
        gCloudTried = true;
        if (gSettings.blinkerKey[0]) {
          // 点灯库的 begin() 会重新 WiFi.begin() 并把 DHCP 主机名改成 DiyArduinoMQTT_xxx，
          // 这里改回来，并直接返回，下一轮再重新读取连接状态
          cloud::begin(gSettings.blinkerKey);
          WiFi.hostname(HOSTNAME);
          return;
        }
        LOG("未设置点灯密钥，不启用 Blinker（本地按键和网页照常可用）");
      }
      if (!gServicesUp && conn) startServices();
      if (gServicesUp) {
        server.handleClient();
        ArduinoOTA.handle();
        if (gOwnMdns) MDNS.update();
      }
      cloud::loop();
      break;
  }

  if (conn != gWasConnected) {
    gWasConnected = conn;
    if (conn) LOG("WiFi 已连接：%s  IP %s", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    else LOG("WiFi 已断开");
  }

  if (gPhase == Phase::Portal) app::setNetLed(app::NetLed::Fast);
  else app::setNetLed(conn ? app::NetLed::On : app::NetLed::Slow);
}

bool connected() { return WiFi.status() == WL_CONNECTED; }

String ip() { return WiFi.localIP().toString(); }

void resetAll() {
  LOG("清除 WiFi 和点灯密钥");
  settingsSetBlinkerKey("");
  gSettings.lanMqttEnable = false;  // 只关掉局域网 MQTT 开关，服务器地址等配置保留
  settingsSave();
  wm.resetSettings();
  app::requestRestart(PSTR("重置配网"));
}

}  // namespace net
