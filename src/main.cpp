// SEMI-AMS 复刻固件
//
// 结构：
//   tick()  每 10ms 由 Ticker 调用（系统定时器上下文）：采样传感器、识别按键、跑状态机、控制电机、LED。
//           网络操作（连接点灯服务器、配网页面等）即使在 loop() 里卡住几秒，tick 仍按时运行，
//           缓冲器满了能及时停电机。
//   loop()  网络、点灯 App、网页、串口命令、日志输出、定时重启。
#include <Arduino.h>
#include <Ticker.h>
#include <Updater.h>
#include <stdarg.h>

#include "app.h"
#include "cloud.h"
#include "config.h"
#include "fsm.h"
#include "input.h"
#include "motor_logic.h"
#include "net.h"
#include "settings.h"

using semi::MotorDir;

namespace {

Ticker gTicker;
semi::Fsm gFsm;
semi::MotorLogic gMotor;

semi::Debouncer gIn(SWITCH_DEBOUNCE), gReady(SWITCH_DEBOUNCE);
semi::Debouncer gLack(HALL_DEBOUNCE), gFull(HALL_DEBOUNCE);
semi::ButtonTracker gBtnStart(BUTTON_DEBOUNCE, false, LONG_PRESS_MS);     // 松开触发
semi::ButtonTracker gBtnEmergency(BUTTON_DEBOUNCE, true, LONG_PRESS_MS);  // 按下立即触发

// 远程命令（loop 里设置，tick 里消费；ESP8266 是协作式调度，不会同时执行）
volatile bool gReqStartStop = false;
volatile bool gReqEmergency = false;
const char* volatile gRestartReason = nullptr;
volatile bool gHaltMotor = false;

volatile MotorDir gJogDir = MotorDir::Stop;
volatile uint8_t gJogSpeed = 0;
volatile uint32_t gJogUntil = 0;

volatile bool gPendingInvert = false;
volatile bool gInvertValue = false;
volatile bool gOtaStopped = false;

volatile app::NetLed gNetLed = app::NetLed::Slow;
volatile uint32_t gUiRev = 0;
uint32_t gLastFsmRev = 0;
MotorDir gLastMotorDir = MotorDir::Stop;

// ---------- 日志环形缓冲 ----------
struct LogEntry {
  uint32_t sec;
  char msg[104];
};
constexpr size_t kLogN = 24;
LogEntry gLogs[kLogN];
uint32_t gLogHead = 0;     // 已写入总数
uint32_t gLogPrinted = 0;  // 已输出到串口的数量

void fsmLogger(const char* m) { app::log("%s", m); }

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

  bool changed = false;
  changed |= gIn.update(digitalRead(PIN_SW_IN) == LOW);
  changed |= gReady.update(digitalRead(PIN_SW_READY) == LOW);
  changed |= gLack.update(digitalRead(PIN_HALL_EMPTY) == LOW);
  changed |= gFull.update(digitalRead(PIN_HALL_FULL) == LOW);
  if (changed) {
    app::log("传感器 进料口:%d 就绪:%d EMPTY:%d FULL:%d", gIn.value(), gReady.value(),
             gLack.value(), gFull.value());
  }

  semi::Commands cmd;
  auto rs = gBtnStart.update(digitalRead(PIN_BTN_START) == LOW, now);
  auto re = gBtnEmergency.update(digitalRead(PIN_BTN_EMERGENCY) == HIGH, now);
  if (rs.shortPress) {
    cmd.startStop = true;
    app::log("黑色按钮：开始/结束");
  }
  if (re.shortPress) {
    cmd.emergency = true;
    app::log("红色按钮：紧急停止/解除");
  }
  if ((rs.longPress || re.longPress) && !gRestartReason) gRestartReason = "长按按键";

  if (gReqStartStop) {
    gReqStartStop = false;
    cmd.startStop = true;
  }
  if (gReqEmergency) {
    gReqEmergency = false;
    cmd.emergency = true;
  }

  // 固件升级（网页或 OTA）时停止电机
  if (Update.isRunning()) {
    if (!gOtaStopped) {
      gOtaStopped = true;
      gFsm.forceEmergency("固件升级中", now);
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
      app::log("点动结束");
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
      "  restart                重启\n"
      "  wifireset              清除 WiFi 和点灯密钥并重启"));
}

void printStatus() {
  app::Status s = app::status();
  Serial.printf("状态：%s  电机：%s %u%%\n", app::stateText().c_str(), semi::Fsm::motorName(s.motor),
                s.duty);
  Serial.printf("进料口:%d 就绪:%d EMPTY:%d FULL:%d\n", s.in, s.ready, s.lack, s.full);
  Serial.printf("运行：%s  WiFi：%s %s  点灯：%s\n", app::uptimeText().c_str(),
                net::connected() ? "已连接" : "未连接", net::ip().c_str(), cloud::statusText());
  size_t n;
  const SettingDef* defs = settingDefs(n);
  for (size_t i = 0; i < n; i++)
    Serial.printf("  %-22s = %ld %s  (%s)\n", defs[i].key, settingsGet(defs[i].key), defs[i].unit,
                  defs[i].label);
  Serial.printf("  blinker_key            = %s\n", gSettings.blinkerKey[0] ? "（已设置）" : "（未设置）");
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
    app::requestStartStop("串口");
  } else if (cmd == "emg") {
    app::requestEmergency("串口");
  } else if (cmd == "restart") {
    app::requestRestart("串口命令");
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
      Serial.printf("%s = %ld 已保存\n", k.c_str(), v);
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

void flushLogs() {
  if (gLogHead - gLogPrinted > kLogN) gLogPrinted = gLogHead - kLogN;
  while (gLogPrinted < gLogHead) {
    const LogEntry& e = gLogs[gLogPrinted % kLogN];
    Serial.printf("[%6lu] %s\n", (unsigned long)e.sec, e.msg);
    gLogPrinted++;
  }
}

}  // namespace

// ================= app:: 实现 =================
namespace app {

void requestStartStop(const char* source) {
  log("%s：开始/结束", source);
  gReqStartStop = true;
}

void requestEmergency(const char* source) {
  log("%s：紧急停止/解除", source);
  gReqEmergency = true;
}

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
  log("点动：%s %u%% %lums", semi::Fsm::motorName(dir), speed, (unsigned long)ms);
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
  s.rev = gUiRev;
  return s;
}

uint64_t uptimeMs() { return micros64() / 1000ULL; }

String uptimeText() {
  uint64_t m = uptimeMs() / 60000ULL;
  char buf[40];
  snprintf(buf, sizeof(buf), "%u天%u时%u分", (unsigned)(m / 1440), (unsigned)((m / 60) % 24),
           (unsigned)(m % 60));
  return String(buf);
}

String stateText() {
  String s = semi::Fsm::stateName(gFsm.state());
  const char* n = gFsm.note();
  if (n && *n) {
    s += "（";
    s += n;
    s += "）";
  }
  return s;
}

void log(const char* fmt, ...) {
  LogEntry& e = gLogs[gLogHead % kLogN];
  e.sec = (uint32_t)(uptimeMs() / 1000ULL);
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
  va_end(ap);
  gLogHead++;
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
  app::log("%s 复刻固件 v%s 启动，输入 help 查看串口命令", FW_NAME, FW_VERSION);

  if (!settingsBegin()) app::log("文件系统挂载失败，使用默认参数");

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
  gBtnEmergency.reset(digitalRead(PIN_BTN_EMERGENCY) == HIGH);
  app::log("传感器 进料口:%d 就绪:%d EMPTY:%d FULL:%d", gIn.value(), gReady.value(), gLack.value(),
           gFull.value());

  // 上电不自动转电机：进料口有耗材时进入"已退料"，按黑色按钮吸料
  gFsm.begin(currentInputs(), millis());
  gTicker.attach_ms(TICK_MS, tick);
  flushLogs();

  net::begin();
}

void loop() {
  net::loop();
  pollSerial();
  flushLogs();

  // 运行满 7 天，空闲时自动重启
  static uint32_t lastCheck = 0;
  if (millis() - lastCheck > 1000) {
    lastCheck = millis();
    uint64_t days = app::uptimeMs() / 86400000ULL;
    if (days >= AUTO_RESTART_DAYS && gFsm.isIdle() && !gRestartReason) {
      app::log("系统已运行%u天，定期自动重启中...", (unsigned)days);
      gRestartReason = "定期重启";
    }
  }

  if (gRestartReason) {
    gHaltMotor = true;
    app::log("系统重启中...（%s）", gRestartReason);
    flushLogs();
    cloud::showRestarting();
    delay(300);  // 等电机刹车完成
    ESP.restart();
  }
}
