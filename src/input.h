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

 private:
  uint8_t samples_;
  bool stable_ = false;
  uint8_t count_ = 0;
};

// 按键：短按 / 长按识别
//   actOnPress = true  时，按下瞬间就触发短按动作（红色急停按钮，越快越好）
//   actOnPress = false 时，松开且按住时间小于长按阈值才触发（黑色按钮，避免和长按重启冲突）
//   长按：按住超过阈值后松开，触发 longPress
class ButtonTracker {
 public:
  struct Result {
    bool shortPress = false;
    bool longPress = false;
  };

  ButtonTracker(uint8_t samples, bool actOnPress, uint32_t longMs)
      : deb_(samples), actOnPress_(actOnPress), longMs_(longMs) {}

  void reset(bool pressed) { deb_.reset(pressed); pressed_ = pressed; ignoreUntilRelease_ = pressed; }

  Result update(bool rawPressed, uint32_t now) {
    Result r;
    if (deb_.update(rawPressed)) {
      bool p = deb_.value();
      if (p) {
        pressed_ = true;
        pressAt_ = now;
        if (actOnPress_ && !ignoreUntilRelease_) r.shortPress = true;
      } else {
        if (pressed_ && !ignoreUntilRelease_) {
          uint32_t held = now - pressAt_;
          if (held >= longMs_) r.longPress = true;
          else if (!actOnPress_) r.shortPress = true;
        }
        pressed_ = false;
        ignoreUntilRelease_ = false;
      }
    }
    return r;
  }

  bool pressed() const { return pressed_; }
  // 正在按住且已超过长按阈值（用来让 LED 提示"松开即重启"）
  bool longHeld(uint32_t now) const {
    return pressed_ && !ignoreUntilRelease_ && (now - pressAt_ >= longMs_);
  }

 private:
  Debouncer deb_;
  bool actOnPress_;
  uint32_t longMs_;
  bool pressed_ = false;
  bool ignoreUntilRelease_ = false;  // 上电时就按着的键，等松开后再识别
  uint32_t pressAt_ = 0;
};

}  // namespace semi
