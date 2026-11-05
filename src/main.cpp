// SEMI-AMS 复刻固件
//
// 结构：
//   tick()  每 10ms 由 Ticker 调用（系统定时器上下文）：采样传感器、识别按键、跑状态机、控制电机、LED。
//           网络操作（连接点灯服务器、配网页面等）即使在 loop() 里卡住几秒，tick 仍按时运行，
//           缓冲器满了能及时停电机。
//   loop()  网络、点灯 App、网页、串口命令、日志输出、定时重启。
#include <Arduino.h>
#include <LittleFS.h>
#include <Ticker.h>
#include <Updater.h>
#include <stdarg.h>

#include "app.h"
#include "cloud.h"
#include "config.h"
#include "crash.h"
#include "fsm.h"
#include "input.h"
#include "motor_logic.h"
#include "net.h"
#include "printer.h"
#include "settings.h"

using semi::MotorDir;

namespace {

Ticker gTicker;
semi::Fsm gFsm;
semi::MotorLogic gMotor;

semi::Debouncer gIn(SWITCH_DEBOUNCE), gReady(SWITCH_DEBOUNCE);
semi::Debouncer gLack(HALL_DEBOUNCE), gFull(HALL_DEBOUNCE);
semi::ButtonTracker gBtnStart(BUTTON_PRESS_SAMPLES, BUTTON_RELEASE_SAMPLES, false, LONG_PRESS_MS);     // 松开触发
semi::ButtonTracker gBtnEmergency(BUTTON_PRESS_SAMPLES, BUTTON_RELEASE_SAMPLES, true, LONG_PRESS_MS);  // 按下立即触发

// 远程命令（loop 里设置，tick 里消费；ESP8266 是协作式调度，不会同时执行）
volatile bool gReqStartStop = false;
volatile bool gReqEmergency = false;
volatile bool gReqPrinterStart = false;
volatile bool gReqPrinterUnload = false;
const char* volatile gRestartReason = nullptr;
volatile bool gHaltMotor = false;

volatile MotorDir gJogDir = MotorDir::Stop;
volatile uint8_t gJogSpeed = 0;
volatile uint32_t gJogUntil = 0;

volatile bool gPendingInvert = false;
volatile bool gInvertValue = false;
volatile bool gOtaStopped = false;

volatile app::NetLed gNetLed = app::NetLed::Slow;

// 黑色按钮的下降沿锁存：触点接触不稳时，每次接通可能只有零点几毫秒（C23 被瞬间放电，
// 再经上拉在 1ms 内充回高电平），10ms 一次的采样大多会错过。中断记下"这 10ms 里接通过"，
// tick 把它当成这一次采样为按下；是否算一次按键仍由 ButtonTracker 的连续次数决定。
volatile bool gStartLowLatch = false;
volatile uint32_t gStartFalls = 0;  // 下降沿累计次数（诊断：按住期间触点断开又接通了几次）

void IRAM_ATTR onStartFall() {
  gStartLowLatch = true;
  gStartFalls = gStartFalls + 1;
}

// 诊断：10ms 周期是否被推迟（loop 里的网络操作长时间不让出 CPU 时会发生，按键可能因此漏检）
volatile uint32_t gTickGapMax = 0;      // 最近 10~20 秒内两次 tick 的最大间隔（ms）
volatile uint32_t gTickGapWin = 0;      // 当前 10 秒窗口内的最大间隔
volatile uint32_t gTickLateCount = 0;   // 间隔 >= TICK_LATE_MS 的累计次数
volatile uint32_t gUiRev = 0;
uint32_t gLastFsmRev = 0;
MotorDir gLastMotorDir = MotorDir::Stop;

// ---------- 日志环形缓冲 ----------
struct LogEntry {
  uint32_t sec;
  char msg[104];
};
constexpr size_t kLogN = 16;  // 内存里只留最近 16 条（更早的在日志文件里）
LogEntry gLogs[kLogN];
uint32_t gLogHead = 0;     // 已写入总数
uint32_t gLogPrinted = 0;  // 已输出到串口的数量
uint32_t gLogFiled = 0;    // 已写进日志文件的数量
uint32_t gLogFileAt = 0;   // 上次写文件的时间

void fsmLogger(const char* m) { LOG("%s", m); }

// ---------- 电机输出 ----------
void applyPin(uint8_t pin, const semi::PinOut& o) {
  switch (o.level) {
    case semi::PinLevel::Low:  digitalWrite(pin, LOW); break;
    case semi::PinLevel::High: digitalWrite(pin, HIGH); break;
    case semi::PinLevel::Pwm:  analogWrite(pin, o.duty); break;  // analogWriteRange = 100
  }
}

void applyMotor(const semi::MotorOutput& out) {
  // 先写固定电平的脚，再写 PWM 脚，避免切换瞬间出现反向脉冲
  if (out.in1.level == semi::PinLevel::Pwm) {
    applyPin(PIN_MOTOR_IN2, out.in2);
    applyPin(PIN_MOTOR_IN1, out.in1);
  } else {
    applyPin(PIN_MOTOR_IN1, out.in1);
    applyPin(PIN_MOTOR_IN2, out.in2);
  }
}

// ---------- LED ----------
void updateLed(uint32_t now) {
  bool on;
  if (gBtnStart.longHeld(now) || gBtnEmergency.longHeld(now)) {
    on = (now / 50) & 1;  // 长按已到时间：急闪，松开即重启
  } else {
    switch (gNetLed) {
      case app::NetLed::On:   on = true; break;
      case app::NetLed::Off:  on = false; break;
      case app::NetLed::Fast: on = (now / 100) & 1; break;  // 配网热点：快闪
      default:                on = (now / 500) & 1; break;  // 连接中：慢闪
    }
  }
  digitalWrite(PIN_LED, on ? LOW : HIGH);
}

semi::Inputs currentInputs() {
  semi::Inputs in;
  in.in = gIn.value();
  in.ready = gReady.value();
  in.lack = gLack.value();
  in.full = gFull.value();
  return in;
}

// ---------- 10ms 周期 ----------
void tick() {
  const uint32_t now = millis();

  {
    static uint32_t lastTickAt = 0, winStart = 0, lastLateLog = 0;
    if (lastTickAt) {
      const uint32_t gap = now - lastTickAt;
      if (gap > gTickGapWin) gTickGapWin = gap;
      if (gap >= TICK_LATE_MS) {
        gTickLateCount = gTickLateCount + 1;
        if (!lastLateLog || now - lastLateLog > 2000) {
          lastLateLog = now | 1;
          LOG("定时任务被推迟 %lums（累计 %lu 次）", (unsigned long)gap, (unsigned long)gTickLateCount);
        }
      }
    }
    lastTickAt = now;
    if (now - winStart >= 10000) {
      gTickGapMax = gTickGapWin;
      gTickGapWin = 0;
      winStart = now;
    }
  }

  const bool swChanged = gIn.update(digitalRead(PIN_SW_IN) == LOW) | gReady.update(digitalRead(PIN_SW_READY) == LOW);
  const bool hallChanged = gLack.update(digitalRead(PIN_HALL_EMPTY) == LOW) | gFull.update(digitalRead(PIN_HALL_FULL) == LOW);
  // 打印中缓冲器的两颗霍尔每隔几十秒就变化一次，不逐条记录（状态机每 10 分钟汇总一次补料情况），
  // 否则一次打印的日志就会把日志文件写满，把开始打印时的记录挤掉
  if (swChanged || (hallChanged && gFsm.state() != semi::State::Printing)) {
    LOG("传感器 进料口:%d 就绪:%d EMPTY:%d FULL:%d", gIn.value(), gReady.value(),
             gLack.value(), gFull.value());
  }

  semi::Commands cmd;
  noInterrupts();
  const bool startLatched = gStartLowLatch;
  gStartLowLatch = false;
  interrupts();
  auto rs = gBtnStart.update(startLatched || digitalRead(PIN_BTN_START) == LOW, now);
  auto re = gBtnEmergency.update(digitalRead(PIN_BTN_EMERGENCY) == HIGH, now);
  // 诊断：记录按键的每一次按下 / 松开
  static bool prevStart = false, prevEmg = false;
  static uint32_t startPressAt = 0, startFallsAtPress = 0;
  if (gBtnStart.pressed() != prevStart) {
    prevStart = gBtnStart.pressed();
    if (prevStart) {
      startPressAt = now;
      startFallsAtPress = gStartFalls;
      LOG("黑色按钮按下");
    } else {
      LOG("黑色按钮松开（按住 %lums，期间触点断开又接通 %lu 次）", (unsigned long)(now - startPressAt),
               (unsigned long)(gStartFalls - startFallsAtPress));
    }
  }
  if (gBtnEmergency.pressed() != prevEmg) {
    prevEmg = gBtnEmergency.pressed();
    LOG("红色按钮" SEMI_PS, prevEmg ? PSTR("按下") : PSTR("松开"));
  }
  if (rs.blip) LOG("黑色按钮信号太短（不到 20ms），未识别");
  if (re.blip) LOG("红色按钮信号太短（不到 20ms），未识别");
  if (rs.shortPress) {
    cmd.startStop = true;
    LOG("黑色按钮：开始/结束");
  }
  if (re.shortPress) {
    cmd.emergency = true;
    LOG("红色按钮：紧急停止/解除");
  }
  if ((rs.longPress || re.longPress) && !gRestartReason) gRestartReason = PSTR("长按按键");

  if (gReqStartStop) {
    gReqStartStop = false;
    cmd.startStop = true;
  }
  if (gReqEmergency) {
    gReqEmergency = false;
    cmd.emergency = true;
  }
  if (gReqPrinterStart) {
    gReqPrinterStart = false;
    cmd.printerStart = true;
  }
  if (gReqPrinterUnload) {
    gReqPrinterUnload = false;
    cmd.printerUnload = true;
  }

  // 固件升级（网页或 OTA）时停止电机
  if (Update.isRunning()) {
    if (!gOtaStopped) {
      gOtaStopped = true;
      gFsm.forceEmergency(PSTR("固件升级中"), now);
    }
  } else {
    gOtaStopped = false;
  }

  // 重启请求由 loop() 执行；万一 loop() 被网络操作卡住超过 5 秒，这里直接重启（电机此时已停）
  static uint32_t restartSeenAt = 0;
  if (gRestartReason) {
    if (!restartSeenAt) restartSeenAt = now | 1;
    else if (now - restartSeenAt > 5000) ESP.restart();
  }

  gFsm.update(currentInputs(), cmd, now);

  // 电机目标
  semi::MotorCmd mc = gFsm.motor();
  if (gHaltMotor || gRestartReason) {
    mc = semi::MotorCmd();
  } else if (gJogUntil) {
    if (gFsm.isIdle() && !cmd.startStop && !cmd.emergency && (int32_t)(gJogUntil - now) > 0) {
      mc.dir = gJogDir;
      mc.speed = gJogSpeed;
    } else {
      gJogUntil = 0;
      LOG("点动结束");
    }
  }

  // 方向反转设置只在电机完全停下时生效，避免运行中直接换向
  if (gPendingInvert && gMotor.dir() == MotorDir::Stop && !gMotor.braking()) {
    gMotor.setInvert(gInvertValue);
    gPendingInvert = false;
  }

  if (gMotor.tick(mc.dir, mc.speed, now)) applyMotor(gMotor.output());

  if (gFsm.revision() != gLastFsmRev || gMotor.dir() != gLastMotorDir) {
    gLastFsmRev = gFsm.revision();
    gLastMotorDir = gMotor.dir();
    gUiRev = gUiRev + 1;
  }

  updateLed(now);
}

// ---------- 串口命令 ----------
String gSerialLine;

void printHelp() {
  Serial.println(F(
      "命令：\n"
      "  status                 查看状态和参数\n"
      "  start                  开始/结束打印（同黑色按钮）\n"
      "  emg                    紧急停止/解除（同红色按钮）\n"
      "  jog f|r <速度%> <毫秒>  空闲时点动电机，确认方向和速度，如 jog f 50 2000\n"
      "  set <参数> <值>        修改参数并保存，如 set feed_speed 55\n"
      "  key <点灯密钥>         设置点灯密钥（重启生效）\n"
      "  mqtt                   查看拓竹打印机连接状态\n"
      "  mqtt on|off            开启/关闭拓竹打印机 MQTT\n"
      "  mqtt ip|sn|code <值>   设置打印机 IP / 序列号 / 访问码\n"
      "  mqtt wait <秒>         打印机退料后最多等多久再退料（0–600）\n"
      "  restart                重启\n"
      "  wifireset              清除 WiFi 和点灯密钥并重启"));
}

void printMqtt() {
  Serial.printf_P(PSTR("拓竹打印机：%s  启用:" SEMI_PS "  IP:%s  序列号:%s  访问码:" SEMI_PS "  退料等待:%us\n"),
                printer::statusText().c_str(), gSettings.mqttEnable ? PSTR("是") : PSTR("否"),
                gSettings.mqttIp[0] ? gSettings.mqttIp : "-", gSettings.mqttSn[0] ? gSettings.mqttSn : "-",
                gSettings.mqttCode[0] ? PSTR("已设置") : "-", (unsigned)gSettings.mqttUnloadWaitS);
  Serial.printf_P(PSTR("  打印机：%s  最近事件：%s\n"), printer::printerText().c_str(), printer::lastEvent());
}

void handleMqttCmd(String rest) {
  int sp = rest.indexOf(' ');
  String sub = sp < 0 ? rest : rest.substring(0, sp);
  String val = sp < 0 ? String() : rest.substring(sp + 1);
  sub.toLowerCase();
  val.trim();
  bool ok = true, changed = true;
  if (!sub.length()) {
    changed = false;
  } else if (sub == "on" || sub == "off") {
    printer::setEnabled(sub == "on");
    changed = false;  // setEnabled 已保存
  } else if (sub == "ip") {
    ok = settingsSetMqttIp(val.c_str());
  } else if (sub == "sn") {
    ok = settingsSetMqttSn(val.c_str());
  } else if (sub == "code") {
    ok = settingsSetMqttCode(val.c_str());
  } else if (sub == "wait") {
    ok = settingsSetMqttUnloadWait(val.toInt());
  } else {
    Serial.println(F("用法：mqtt [on|off|ip <IP>|sn <序列号>|code <访问码>|wait <秒>]"));
    return;
  }
  if (!ok) {
    Serial.println(F("格式不对：IP 如 192.168.1.50；序列号 8–20 位字母数字；访问码 6–16 位字母数字；wait 0–600"));
    return;
  }
  if (changed) {
    settingsSave();
    printer::applySettings();
    Serial.println(F("已保存"));
  }
  printMqtt();
}

void printStatus() {
  app::Status s = app::status();
  Serial.printf_P(PSTR("状态：%s  电机：" SEMI_PS " %u%%\n"), app::stateText().c_str(), semi::Fsm::motorName(s.motor),
                s.duty);
  Serial.printf_P(PSTR("进料口:%d 就绪:%d EMPTY:%d FULL:%d\n"), s.in, s.ready, s.lack, s.full);
  Serial.printf_P(PSTR("黑色按钮:" SEMI_PS "（GPIO0=" SEMI_PS "）  红色按钮:" SEMI_PS "（GPIO15=" SEMI_PS "）\n"),
                s.btnStart ? PSTR("按下") : PSTR("松开"), s.rawGpio0 ? PSTR("高") : PSTR("低"),
                s.btnEmg ? PSTR("按下") : PSTR("松开"), s.rawGpio15 ? PSTR("高") : PSTR("低"));
  Serial.printf_P(PSTR("定时任务：最近最大间隔 %lums，被推迟（>=%dms）累计 %lu 次\n"), (unsigned long)s.tickGapMs,
                TICK_LATE_MS, (unsigned long)s.tickLate);
  Serial.printf_P(PSTR("运行：%s  WiFi：" SEMI_PS " %s  点灯：" SEMI_PS "\n"), app::uptimeText().c_str(),
                net::connected() ? PSTR("已连接") : PSTR("未连接"), net::ip().c_str(), cloud::statusText());
  size_t n;
  const SettingDef* defs = settingDefs(n);
  for (size_t i = 0; i < n; i++)
    Serial.printf_P(PSTR("  %-22s = %ld %s  (" SEMI_PS ")\n"), defs[i].key, settingsGet(defs[i].key), defs[i].unit,
                  defs[i].label);
  Serial.printf_P(PSTR("  blinker_key            = " SEMI_PS "\n"), gSettings.blinkerKey[0] ? PSTR("（已设置）") : PSTR("（未设置）"));
  printMqtt();
}

void handleSerialLine(String line) {
  line.trim();
  if (!line.length()) return;
  int sp = line.indexOf(' ');
  String cmd = sp < 0 ? line : line.substring(0, sp);
  String rest = sp < 0 ? String() : line.substring(sp + 1);
  rest.trim();
  cmd.toLowerCase();

  if (cmd == "help" || cmd == "?") {
    printHelp();
  } else if (cmd == "status") {
    printStatus();
  } else if (cmd == "start") {
    app::requestStartStop(PSTR("串口"));
  } else if (cmd == "emg") {
    app::requestEmergency(PSTR("串口"));
  } else if (cmd == "restart") {
    app::requestRestart(PSTR("串口命令"));
  } else if (cmd == "mqtt") {
    handleMqttCmd(rest);
  } else if (cmd == "wifireset") {
    net::resetAll();
  } else if (cmd == "key") {
    settingsSetBlinkerKey(rest.c_str());
    settingsSave();
    Serial.println(F("点灯密钥已保存，重启后生效"));
  } else if (cmd == "set") {
    int sp2 = rest.indexOf(' ');
    if (sp2 < 0) {
      Serial.println(F("用法：set <参数> <值>"));
      return;
    }
    String k = rest.substring(0, sp2);
    long v = rest.substring(sp2 + 1).toInt();
    if (settingsSet(k.c_str(), v)) {
      settingsSave();
      app::applySettings();
      Serial.printf_P(PSTR("%s = %ld 已保存\n"), k.c_str(), v);
    } else {
      Serial.println(F("参数名错误或超出范围，输入 status 查看"));
    }
  } else if (cmd == "jog") {
    char d = rest.length() ? rest[0] : 'f';
    long spd = 50, ms = 2000;
    sscanf(rest.c_str() + (rest.length() > 1 ? 1 : 0), "%ld %ld", &spd, &ms);
    spd = constrain(spd, 10L, 100L);
    ms = constrain(ms, 100L, (long)JOG_MAX_MS);
    if (!app::requestJog(d == 'r' ? MotorDir::Reverse : MotorDir::Forward, (uint8_t)spd, (uint32_t)ms))
      Serial.println(F("只有在 无耗材/耗材就绪/已退料 状态下才能点动"));
  } else {
    Serial.println(F("未知命令，输入 help 查看"));
  }
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      handleSerialLine(gSerialLine);
      gSerialLine = "";
    } else if (gSerialLine.length() < 120) {
      gSerialLine += c;
    }
  }
}

// 日志同时写进 LittleFS 的 /log.txt（超过 48KB 换到 /log1.txt），重启后也能看，网页可以下载。
// 攒一批再写（10 秒一次或攒够 8 条），减少 Flash 擦写；只在 loop() 里写，tick 里只写内存
void fileLogs(bool force) {
  const uint32_t pending = gLogHead - gLogFiled;
  if (!pending) return;
  if (!force && pending < 8 && millis() - gLogFileAt < 10000) return;
  gLogFileAt = millis();
  File f = LittleFS.open(app::kLogFile, "a");
  if (!f) {
    gLogFiled = gLogHead;
    return;
  }
  if (pending > kLogN) {
    f.printf_P(PSTR("（丢失 %lu 条日志）\n"), (unsigned long)(pending - kLogN));
    gLogFiled = gLogHead - kLogN;
  }
  while (gLogFiled < gLogHead) {
    const LogEntry& e = gLogs[gLogFiled % kLogN];
    f.printf("[%6lu] %s\n", (unsigned long)e.sec, e.msg);
    gLogFiled++;
  }
  const size_t size = f.size();
  f.close();
  if (size > 48 * 1024) {
    LittleFS.remove(app::kLogFileOld);
    LittleFS.rename(app::kLogFile, app::kLogFileOld);
  }
}

void flushLogs() {
  if (gLogHead - gLogPrinted > kLogN) gLogPrinted = gLogHead - kLogN;
  while (gLogPrinted < gLogHead) {
    const LogEntry& e = gLogs[gLogPrinted % kLogN];
    Serial.printf_P(PSTR("[%6lu] %s\n"), (unsigned long)e.sec, e.msg);
    gLogPrinted++;
  }
}

}  // namespace

// ================= app:: 实现 =================
namespace app {

void requestStartStop(const char* source) {
  LOG(SEMI_PS "：开始/结束", source);
  gReqStartStop = true;
}

void requestEmergency(const char* source) {
  LOG(SEMI_PS "：紧急停止/解除", source);
  gReqEmergency = true;
}

void requestPrinterStart(const char* reason) {
  LOG(SEMI_PS, reason);
  gReqPrinterUnload = false;
  gReqPrinterStart = true;
}

void requestPrinterUnload(const char* reason) {
  LOG(SEMI_PS, reason);
  gReqPrinterStart = false;
  gReqPrinterUnload = true;
}

bool motorStopped() { return gMotor.dir() == MotorDir::Stop && !gMotor.braking(); }

void requestRestart(const char* reason) {
  if (!gRestartReason) gRestartReason = reason;
}

bool requestJog(MotorDir dir, uint8_t speed, uint32_t ms) {
  if (!gFsm.isIdle()) return false;
  if (speed < 10) speed = 10;
  if (speed > 100) speed = 100;
  if (ms > JOG_MAX_MS) ms = JOG_MAX_MS;
  gJogDir = dir;
  gJogSpeed = speed;
  gJogUntil = millis() + ms;
  LOG("点动：" SEMI_PS " %u%% %lums", semi::Fsm::motorName(dir), speed, (unsigned long)ms);
  return true;
}

void applySettings() {
  gFsm.setParams(gSettings.p);
  gInvertValue = gSettings.motorInvert;
  gPendingInvert = true;
}

Status status() {
  Status s;
  s.state = gFsm.state();
  s.note = gFsm.note();
  s.motor = gMotor.dir();
  s.duty = gMotor.duty();
  s.in = gIn.value();
  s.ready = gReady.value();
  s.lack = gLack.value();
  s.full = gFull.value();
  s.btnStart = gBtnStart.pressed();
  s.btnEmg = gBtnEmergency.pressed();
  s.rawGpio0 = digitalRead(PIN_BTN_START) == HIGH;
  s.rawGpio15 = digitalRead(PIN_BTN_EMERGENCY) == HIGH;
  s.tickGapMs = gTickGapMax > gTickGapWin ? gTickGapMax : gTickGapWin;
  s.tickLate = gTickLateCount;
  s.rev = gUiRev;
  return s;
}

uint64_t uptimeMs() { return micros64() / 1000ULL; }

String uptimeText() {
  uint64_t m = uptimeMs() / 60000ULL;
  char buf[40];
  snprintf_P(buf, sizeof(buf), PSTR("%u天%u时%u分"), (unsigned)(m / 1440), (unsigned)((m / 60) % 24),
           (unsigned)(m % 60));
  return String(buf);
}

String stateText() {
  // 状态名和备注在 Flash 里（ESP8266），不能直接逐字节读
  String s(FPSTR(semi::Fsm::stateName(gFsm.state())));
  const char* n = gFsm.note();
  if (n && pgm_read_byte(n)) {
    s += F("（");
    s += FPSTR(n);
    s += F("）");
  }
  return s;
}

namespace {
// fmt 在 Flash 里时 pgm = true
void vlogEntry(bool pgm, const char* fmt, va_list ap) {
  LogEntry& e = gLogs[gLogHead % kLogN];
  e.sec = (uint32_t)(uptimeMs() / 1000ULL);
  const int n = pgm ? vsnprintf_P(e.msg, sizeof(e.msg), fmt, ap) : vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  if (n >= (int)sizeof(e.msg)) {
    // 被截断时不要留下半个汉字：找到最后一个字符的起始字节，字节数不够就去掉它
    const size_t len = strlen(e.msg);
    size_t start = len;
    while (start && ((uint8_t)e.msg[start - 1] & 0xC0) == 0x80) start--;
    if (start) {
      const uint8_t lead = (uint8_t)e.msg[start - 1];
      const size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
      if (len - (start - 1) < need) e.msg[start - 1] = 0;
    }
  }
  gLogHead++;
}
}  // namespace

void log(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vlogEntry(false, fmt, ap);
  va_end(ap);
}

void logP(const char* fmtP, ...) {
  va_list ap;
  va_start(ap, fmtP);
  vlogEntry(true, fmtP, ap);
  va_end(ap);
}

size_t logCount() { return gLogHead < kLogN ? gLogHead : kLogN; }

String logLine(size_t i) {
  size_t n = logCount();
  if (i >= n) return String();
  const LogEntry& e = gLogs[(gLogHead - n + i) % kLogN];
  char head[16];
  snprintf(head, sizeof(head), "[%lu] ", (unsigned long)e.sec);
  return String(head) + e.msg;
}

void setNetLed(NetLed m) { gNetLed = m; }

}  // namespace app

// ================= Arduino 入口 =================
void setup() {
  // 先把电机引脚拉低（DRV8870 滑行 + 休眠），再做其它事
  digitalWrite(PIN_MOTOR_IN1, LOW);
  digitalWrite(PIN_MOTOR_IN2, LOW);
  pinMode(PIN_MOTOR_IN1, OUTPUT);
  pinMode(PIN_MOTOR_IN2, OUTPUT);
  pinMode(PIN_LED, OUTPUT);
  digitalWrite(PIN_LED, HIGH);

  pinMode(PIN_HALL_EMPTY, INPUT);   // DRV5032FA 推挽输出，不需要上拉
  pinMode(PIN_HALL_FULL, INPUT);
  pinMode(PIN_SW_IN, INPUT_PULLUP);  // 微动开关接地，板上没有外部上拉
  pinMode(PIN_SW_READY, INPUT_PULLUP);
  pinMode(PIN_BTN_START, INPUT_PULLUP);
  pinMode(PIN_BTN_EMERGENCY, INPUT);  // 模组内部已下拉，按下经 R14 拉高

  analogWriteRange(100);
  analogWriteFreq(MOTOR_PWM_HZ);

  Serial.begin(115200);
  Serial.println();
  LOG("==== %s v%s 启动（重启原因：%s）", FW_NAME, FW_VERSION, ESP.getResetReason().c_str());
  {
    // 闪存芯片的实际容量（按芯片 ID）和固件按多大编译。实际比固件小时，网页升级和 OTA 都会被
    // 拒绝（Flash config wrong），只能用串口烧录
    uint32_t realKb = ESP.getFlashChipRealSize() / 1024, sdkKb = ESP.getFlashChipSize() / 1024;
    LOG("闪存：ID %06X，实际 %u KB，固件按 %u KB" SEMI_PS, (unsigned)ESP.getFlashChipId(),
             (unsigned)realKb, (unsigned)sdkKb,
             realKb < sdkKb ? PSTR("（芯片偏小，只能串口烧录）") : "");
  }
  crash::reportLast();

  if (!settingsBegin()) LOG("文件系统挂载失败，使用默认参数");

  semi::MotorLogic::Config mcfg;
  mcfg.startDuty = MOTOR_START_DUTY;
  mcfg.rampStep = MOTOR_RAMP_STEP;
  mcfg.brakeMs = MOTOR_BRAKE_MS;
  gMotor.setConfig(mcfg);
  gMotor.setInvert(gSettings.motorInvert);
  gFsm.setParams(gSettings.p);
  gFsm.setLogger(fsmLogger);

  // 用当前电平初始化消抖器；上电时就按着的按键要等松开后才识别
  gIn.reset(digitalRead(PIN_SW_IN) == LOW);
  gReady.reset(digitalRead(PIN_SW_READY) == LOW);
  gLack.reset(digitalRead(PIN_HALL_EMPTY) == LOW);
  gFull.reset(digitalRead(PIN_HALL_FULL) == LOW);
  gBtnStart.reset(digitalRead(PIN_BTN_START) == LOW);
  attachInterrupt(digitalPinToInterrupt(PIN_BTN_START), onStartFall, FALLING);
  gBtnEmergency.reset(digitalRead(PIN_BTN_EMERGENCY) == HIGH);
  LOG("传感器 进料口:%d 就绪:%d EMPTY:%d FULL:%d", gIn.value(), gReady.value(), gLack.value(),
           gFull.value());

  // 上电不自动转电机：进料口有耗材时进入"已退料"，按黑色按钮吸料
  gFsm.begin(currentInputs(), millis());
  gTicker.attach_ms(TICK_MS, tick);
  flushLogs();

  printer::begin();
  net::begin();
}

void loop() {
  crash::noteHeap(ESP.getFreeHeap());
  net::loop();
  printer::loop();
  pollSerial();
  flushLogs();
  fileLogs(false);

  // 运行满 7 天，空闲时自动重启
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    uint64_t days = app::uptimeMs() / 86400000ULL;
    if (days >= AUTO_RESTART_DAYS && gFsm.isIdle() && !gRestartReason) {
      LOG("系统已运行%u天，定期自动重启中...", (unsigned)days);
      gRestartReason = PSTR("定期重启");
    }
  }

  if (gRestartReason) {
    gHaltMotor = true;
    LOG("系统重启中...（" SEMI_PS "）", gRestartReason);
    flushLogs();
    fileLogs(true);
    cloud::showRestarting();
    delay(300);  // 等电机刹车完成
    ESP.restart();
  }
}
