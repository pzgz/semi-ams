// 输入消抖与按键长短按识别（纯 C++，可在电脑上测试）
#pragma once
#include <stdint.h>

namespace semi {

// 连续 n 次采样一致才改变状态
class Debouncer {
 public:
  explicit Debouncer(uint8_t samples = 3) : samples_(samples) {}
  void reset(bool v) { stable_ = v; count_ = 0; }
  // 返回 true 表示稳定状态发生了变化
  bool update(bool raw) {
    if (raw == stable_) {
      if (count_) blip_ = count_;  // 变了几次又变回来，没达到确认次数
      count_ = 0;
      return false;
    }
    if (++count_ >= samples_) {
      stable_ = raw;
      count_ = 0;
      return true;
    }
    return false;
  }
  bool value() const { return stable_; }
  // 最近一次"变了又很快变回来"持续的采样次数（诊断开关接触不良），读一次就清零；0 = 没有
  uint8_t takeBlip() {
    const uint8_t b = blip_;
    blip_ = 0;
    return b;
  }

 private:
  uint8_t samples_;
  bool stable_ = false;
  uint8_t count_ = 0;
  uint8_t blip_ = 0;
};

// 按键：短按 / 长按识别
//   按下和松开的确认次数不同：
//     按下：连续 pressSamples 次为按下就确认（默认 2 次 = 20ms，快速点按也能识别）
//     松开：连续 releaseSamples 次为松开才确认（默认 5 次 = 50ms），
//           按住期间触点短暂接触不良（震动、按得轻）不会被当成"松开再按"，避免一次按压触发两次
//   actOnPress = true  时，按下瞬间就触发短按动作（红色急停按钮，越快越好）
//   actOnPress = false 时，松开且按住时间小于长按阈值才触发（黑色按钮，避免和长按重启冲突）
//   长按：按住超过阈值后松开，触发 longPress
//   blip：信号变成按下又很快恢复，没达到确认次数（用于诊断按键接触不良）
class ButtonTracker {
 public:
  struct Result {
    bool shortPress = false;
    bool longPress = false;
    bool blip = false;
    uint8_t blipSamples = 0;  // blip 时信号为按下的采样次数（× 采样周期 ≈ 持续时间）
  };

  ButtonTracker(uint8_t pressSamples, uint8_t releaseSamples, bool actOnPress, uint32_t longMs)
      : pressSamples_(pressSamples), releaseSamples_(releaseSamples), actOnPress_(actOnPress), longMs_(longMs) {}

  void reset(bool pressed) {
    pressed_ = pressed;
    ignoreUntilRelease_ = pressed;
    count_ = 0;
  }

  Result update(bool rawPressed, uint32_t now) {
    Result r;
    if (!pressed_) {
      if (rawPressed) {
        if (++count_ >= pressSamples_) {
          count_ = 0;
          pressed_ = true;
          pressAt_ = now;
          if (actOnPress_ && !ignoreUntilRelease_) r.shortPress = true;
        }
      } else {
        if (count_ > 0) {
          r.blip = true;
          r.blipSamples = count_;
        }
        count_ = 0;
      }
    } else {
      if (!rawPressed) {
        if (++count_ >= releaseSamples_) {
          count_ = 0;
          if (!ignoreUntilRelease_) {
            uint32_t held = now - pressAt_;
            if (held >= longMs_) r.longPress = true;
            else if (!actOnPress_) r.shortPress = true;
          }
          pressed_ = false;
          ignoreUntilRelease_ = false;
        }
      } else {
        count_ = 0;
      }
    }
    return r;
  }

  bool pressed() const { return pressed_; }
  uint8_t pressSamples() const { return pressSamples_; }
  // 正在按住且已超过长按阈值（用来让 LED 提示"松开即重启"）
  bool longHeld(uint32_t now) const {
    return pressed_ && !ignoreUntilRelease_ && (now - pressAt_ >= longMs_);
  }

 private:
  uint8_t pressSamples_;
  uint8_t releaseSamples_;
  bool actOnPress_;
  uint32_t longMs_;
  bool pressed_ = false;
  bool ignoreUntilRelease_ = false;  // 上电时就按着的键，等松开后再识别
  uint8_t count_ = 0;
  uint32_t pressAt_ = 0;
};

}  // namespace semi
