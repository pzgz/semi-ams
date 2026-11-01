// 各模块共用的接口（实现在 main.cpp）
#pragma once
#include <Arduino.h>

#include "fsm.h"

namespace app {

// 远程命令（App / 网页 / 串口），在下一个 10ms 周期由状态机处理
void requestStartStop(const char* source);
void requestEmergency(const char* source);
void requestRestart(const char* reason);
// 手动点动（只在空闲时允许），用于确认电机方向和速度
bool requestJog(semi::MotorDir dir, uint8_t speed, uint32_t ms);
// 参数修改后应用到状态机和电机
void applySettings();

struct Status {
  semi::State state;
  const char* note;
  semi::MotorDir motor;  // 实际电机方向（含点动）
  uint8_t duty;
  bool in, ready, lack, full;
  uint32_t rev;  // 状态有变化时改变
};
Status status();

uint64_t uptimeMs();
String uptimeText();  // x天x时x分
String stateText();   // 状态 + 备注

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
size_t logCount();
String logLine(size_t i);  // 0 = 最旧

enum class NetLed : uint8_t { Off, Slow, Fast, On };
void setNetLed(NetLed m);

}  // namespace app
