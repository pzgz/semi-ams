// 各模块共用的接口（实现在 main.cpp）
#pragma once
#include <Arduino.h>

#include "fsm.h"
#include "pgm.h"

namespace app {

// 远程命令（App / 网页 / 串口），在下一个 10ms 周期由状态机处理
// source / reason 可以是 Flash 字符串（PSTR），日志里用 %S 打印
void requestStartStop(const char* source);
void requestEmergency(const char* source);
void requestRestart(const char* reason);
// 拓竹打印机（MQTT）事件：开始打印（外置料盘）/ 打印机已退料
void requestPrinterStart(const char* reason);
void requestPrinterUnload(const char* reason);
bool motorStopped();  // 电机完全停止（不在运转也不在刹车）
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
  bool btnStart, btnEmg;          // 按键消抖后的状态：true = 按下
  bool rawGpio0, rawGpio15;       // 按键引脚当前电平（黑键 GPIO0 按下为低，红键 GPIO15 按下为高）
  uint32_t tickGapMs;             // 最近 10~20 秒内 10ms 周期的最大间隔
  uint32_t tickLate;              // 周期被推迟（>= TICK_LATE_MS）的累计次数
  uint32_t rev;  // 状态有变化时改变
};
Status status();

uint64_t uptimeMs();
String uptimeText();  // x天x时x分
String stateText();   // 状态 + 备注

void log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
// 格式字符串在 Flash 里（PSTR）的版本。ESP8266 的字符串常量默认放在内存里，日志里的中文很多，
// 全放内存要占好几 KB；内存不够时点灯、mDNS 申请内存失败会直接崩溃重启。请用下面的 LOG()
void logP(const char* fmtP, ...);
// 只在电脑上做语法检查时检查格式：ESP8266 上 Flash 字符串用 %S（SEMI_PS），编译器会误报
#if defined(ARDUINO)
inline void logCheck(const char*, ...) {}
#else
inline void logCheck(const char*, ...) __attribute__((format(printf, 1, 2)));
inline void logCheck(const char*, ...) {}
#endif
// 日志文件（LittleFS）：新的在 kLogFile，写满 48KB 后旧的挪到 kLogFileOld
constexpr const char* kLogFile = "/log.txt";
constexpr const char* kLogFileOld = "/log1.txt";
size_t logCount();
String logLine(size_t i);  // 0 = 最旧

enum class NetLed : uint8_t { Off, Slow, Fast, On };
void setNetLed(NetLed m);

}  // namespace app

// 写一条日志：LOG("格式", 参数...)。格式必须是字符串常量，放在 Flash 里不占内存；
// if (false) 那一句只是让编译器照常检查格式和参数是否匹配，不会生成代码
#define LOG(fmt, ...)                                \
  do {                                               \
    if (false) ::app::logCheck(fmt, ##__VA_ARGS__);  \
    ::app::logP(PSTR(fmt), ##__VA_ARGS__);           \
  } while (0)
