// SEMI-AMS 状态机（纯 C++，不依赖 Arduino，可在电脑上做仿真测试）
//
// 设计原则：所有传感器都按"当前电平"判断，不依赖边沿。
// 即使某一次变化被错过，下一个 10ms 周期也会按实际电平纠正，
// 不会再出现"要拿开再靠近才能触发"的问题。
#pragma once
#include <stdint.h>

namespace semi {

enum class State : uint8_t {
  NoMaterial,  // 无耗材
  Loading,     // 耗材吸入中
  Ready,       // 耗材就绪
  Printing,    // 打印中（缓冲器自动补料）
  Unloading,   // 退料中
  Retracted,   // 已退料：耗材退到就绪开关之后，停在进料口里，按黑色按钮会重新吸料
  Emergency,   // 紧急停止
};

enum class MotorDir : uint8_t { Stop, Forward, Reverse };

// 已消抖的传感器状态，true = 有效
struct Inputs {
  bool in = false;     // IN_CHECK 微动开关：进料口有耗材
  bool ready = false;  // READY_CHECK 微动开关：耗材已到就绪位置
  bool lack = false;   // EMPTY 霍尔：缓冲器空（需要补料）
  bool full = false;   // FULL 霍尔：缓冲器满（停止补料）
};

// 一次性命令（按钮短按 / App / 网页 / 串口）
struct Commands {
  bool startStop = false;  // 黑色按钮：开始 / 结束打印
  bool emergency = false;  // 红色按钮：紧急停止 / 解除
};

struct Params {
  uint8_t loadSpeed = 50;    // 吸入速度 %
  uint8_t feedSpeed = 60;    // 打印补料速度 %
  uint8_t unloadSpeed = 60;  // 退料速度 %
  uint32_t insertDelayMs = 500;        // 插入耗材后等待多久开始吸入
  uint32_t loadTimeoutMs = 30000;      // 吸入超时
  uint32_t firstFeedTimeoutMs = 120000;  // 开始打印后第一次送料（要走完整条 PTFE 管）超时
  uint32_t feedTimeoutMs = 30000;      // 打印中每次补料超时
  uint32_t unloadTimeoutMs = 90000;    // 退料超时
  uint32_t unloadExtraMs = 300;        // 退料时耗材离开就绪开关后再多退的时间
};

struct MotorCmd {
  MotorDir dir = MotorDir::Stop;
  uint8_t speed = 0;  // %
};

typedef void (*LogFn)(const char* msg);

class Fsm {
 public:
  void setParams(const Params& p) { params_ = p; }
  const Params& params() const { return params_; }
  void setLogger(LogFn fn) { log_ = fn; }

  // 上电：就绪开关有耗材 -> 耗材就绪；只有进料口有耗材 -> 已退料（按黑色按钮吸料）；否则无耗材
  void begin(const Inputs& in, uint32_t now);
  void update(const Inputs& in, const Commands& cmd, uint32_t now);
  // 外部强制停机（例如固件升级）
  void forceEmergency(const char* reason, uint32_t now);

  State state() const { return state_; }
  MotorCmd motor() const { return motor_; }
  const char* note() const { return note_; }
  bool feeding() const { return feeding_; }
  // 空闲：可以安全重启 / 手动点动
  bool isIdle() const {
    return (state_ == State::NoMaterial || state_ == State::Ready || state_ == State::Retracted) &&
           motor_.dir == MotorDir::Stop;
  }
  // 任何用户可见的变化都会让它加一，用于判断是否需要刷新 App
  uint32_t revision() const { return rev_; }

  static const char* stateName(State s);  // 中文名（App 显示）
  static const char* stateKey(State s);   // 英文名（日志）
  static const char* motorName(MotorDir d);

 private:
  void enter(State s, uint32_t now, const char* why);
  void emergency(const char* reason, uint32_t now);
  void setNote(const char* n);
  void setMotor(MotorDir d, uint8_t speed);
  void logf(const char* fmt, ...);

  Params params_;
  LogFn log_ = nullptr;
  Inputs last_;  // 最近一次的传感器状态（进入新状态时用）

  State state_ = State::NoMaterial;
  State resume_ = State::NoMaterial;  // 解除紧急停止后回到的状态
  MotorCmd motor_;
  const char* note_ = "";
  uint32_t rev_ = 0;
  uint32_t enteredAt_ = 0;

  // NoMaterial：需要先看到"进料口没有耗材"，才允许下一次自动吸入，
  // 防止用户往外抽耗材时被电机又吸回去
  bool armed_ = false;
  bool inTiming_ = false;
  uint32_t inSince_ = 0;

  // Loading：满料霍尔只有在吸入过程中"新触发"才当作就绪（防止滑块静止时本来就停在 FULL）
  bool fullArmed_ = false;
  bool startAfterLoad_ = false;  // 吸料完成后直接开始打印（吸料过程中按了黑色按钮，或从"已退料"按黑色按钮）
  bool readyBySwitch_ = false;   // 就绪状态是由就绪开关确认的（不是满料霍尔兜底）

  // Printing
  bool feeding_ = false;
  uint32_t feedStart_ = 0;
  uint32_t feedLimit_ = 0;
  bool outOfMaterial_ = false;
  bool refillTiming_ = false;  // 打印中耗材用尽后又插入新耗材，等插入延时
  uint32_t refillSince_ = 0;

  // Unloading
  bool readySeen_ = false;  // 本次退料中就绪开关有过耗材；从没有过时（开关失效）一直退到离开进料口
  bool clearing_ = false;
  uint32_t clearSince_ = 0;
};

}  // namespace semi
