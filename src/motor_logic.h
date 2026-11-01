// DRV8870 电机控制逻辑（纯 C++，可在电脑上测试）
//
// DRV8870 真值表：
//   IN1 IN2
//    0   0   滑行（并进入低功耗）
//    1   0   OUT1 -> OUT2（物理正向）
//    0   1   OUT2 -> OUT1（物理反向）
//    1   1   刹车（低边导通，慢衰减）
//
// 调速用"慢衰减"方式：一路保持高电平，另一路输出反相 PWM，
// 在"驱动"和"刹车"之间切换，低速时力矩和线性都比滑行方式好。
// 停止时先刹车一小段时间再滑行，送料停得更干脆；换向必须先经过刹车。
#pragma once
#include <stdint.h>

#include "fsm.h"

namespace semi {

enum class PinLevel : uint8_t { Low, High, Pwm };

struct PinOut {
  PinLevel level = PinLevel::Low;
  uint8_t duty = 0;  // level == Pwm 时，高电平占比 %
  bool operator==(const PinOut& o) const { return level == o.level && duty == o.duty; }
  bool operator!=(const PinOut& o) const { return !(*this == o); }
};

struct MotorOutput {
  PinOut in1, in2;
  bool operator==(const MotorOutput& o) const { return in1 == o.in1 && in2 == o.in2; }
  bool operator!=(const MotorOutput& o) const { return !(*this == o); }
};

class MotorLogic {
 public:
  struct Config {
    uint8_t startDuty = 25;  // 起步占空比 %
    uint8_t rampStep = 3;    // 每个 tick 增加的占空比 %
    uint32_t brakeMs = 150;  // 停止 / 换向时的刹车时间
  };

  void setConfig(const Config& c) { cfg_ = c; }
  void setInvert(bool inv) { invert_ = inv; }
  bool invert() const { return invert_; }

  // 每个 tick 调用一次；返回 true 表示引脚输出需要更新
  bool tick(MotorDir target, uint8_t speed, uint32_t now) {
    if (speed > 100) speed = 100;
    if (speed == 0) target = MotorDir::Stop;
    MotorOutput before = out_;

    if (target == MotorDir::Stop) {
      if (phase_ == Phase::Run) startBrake(now);
      else if (phase_ == Phase::Brake && reached(now)) coast();
    } else {
      if (phase_ == Phase::Run && dir_ != target) {
        startBrake(now);
      } else if (phase_ == Phase::Brake) {
        if (reached(now)) coast();
      } else if (phase_ == Phase::Idle) {
        phase_ = Phase::Run;
        dir_ = target;
        duty_ = speed < cfg_.startDuty ? speed : cfg_.startDuty;
        drive();
      } else {  // Run，同方向：加速到目标或直接降到目标
        if (duty_ < speed) {
          uint16_t d = duty_ + cfg_.rampStep;
          duty_ = d > speed ? speed : (uint8_t)d;
        } else {
          duty_ = speed;
        }
        drive();
      }
    }
    return out_ != before;
  }

  MotorOutput output() const { return out_; }
  // 实际方向（逻辑方向，已考虑反转设置之前）
  MotorDir dir() const { return phase_ == Phase::Run ? dir_ : MotorDir::Stop; }
  uint8_t duty() const { return phase_ == Phase::Run ? duty_ : 0; }
  bool braking() const { return phase_ == Phase::Brake; }

 private:
  enum class Phase : uint8_t { Idle, Brake, Run };

  bool reached(uint32_t now) const { return (int32_t)(now - brakeUntil_) >= 0; }

  void startBrake(uint32_t now) {
    phase_ = Phase::Brake;
    brakeUntil_ = now + cfg_.brakeMs;
    duty_ = 0;
    out_.in1 = {PinLevel::High, 0};
    out_.in2 = {PinLevel::High, 0};
  }

  void coast() {
    phase_ = Phase::Idle;
    dir_ = MotorDir::Stop;
    out_.in1 = {PinLevel::Low, 0};
    out_.in2 = {PinLevel::Low, 0};
  }

  void drive() {
    bool physForward = (dir_ == MotorDir::Forward) != invert_;
    uint8_t brakeShare = 100 - duty_;  // 反相 PWM：高电平 = 刹车
    PinOut held = {PinLevel::High, 0};
    PinOut pwm = brakeShare == 0 ? PinOut{PinLevel::Low, 0} : PinOut{PinLevel::Pwm, brakeShare};
    if (physForward) {
      out_.in1 = held;
      out_.in2 = pwm;
    } else {
      out_.in1 = pwm;
      out_.in2 = held;
    }
  }

  Config cfg_;
  bool invert_ = false;
  Phase phase_ = Phase::Idle;
  MotorDir dir_ = MotorDir::Stop;
  uint8_t duty_ = 0;
  uint32_t brakeUntil_ = 0;
  MotorOutput out_;
};

}  // namespace semi
