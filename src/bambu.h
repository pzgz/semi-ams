// 解读拓竹打印机的 MQTT 消息（device/<序列号>/report），决定什么时候自动送料、什么时候自动退料。
// 纯 C++，可在电脑上测试；网络部分在 printer.cpp。
//
// 判断依据（参考原厂 MQTT 版固件用到的字段）：
//   开始送料：打印机回报 project_file 命令（开始打印），并且 use_ams 为 false，
//            或 ams_mapping2 里有外置料盘（ams_id 254/255 且 slot_id 不是 255）。
//            备用：打印状态从非打印变成 PREPARE/RUNNING 时，当前用的是外置料盘。
//   开始退料：打印机回报 ams_change_filament 且 target=255、slot_id 不存在或为 255（退料），
//            或 unload_filament。新协议打印机（P2S、H2 系列）从 Bambu Studio 进外置料盘时
//            也发 target=255，但 slot_id 是 0，不能当成退料。
//            为了不和打印机的挤出机对拉，要等打印机切断耗材、把耗材退出它自己的传感器再退；
//            等不到就在 unloadWaitMs 后退。
//            备用：打印机空闲时，它的耗材传感器从"有"变成"无"（在屏幕上退料、或手动拔出耗材）。
//   当前料盘和耗材传感器：新协议打印机看 device.extruder.info[]（snow 的高 8 位是料盘组，
//            254/255 = 外置料盘；info 的 bit1 = 挤出机里有耗材）；老协议看 ams.tray_now（254 = 外置）
//            和 hw_switch_state 的 bit0。
//   每条消息里没出现的字段保持上一次的值（P 系列只发送有变化的字段）。
#pragma once
#include <stddef.h>
#include <stdint.h>

#include "json_scan.h"

namespace semi {

class BambuMonitor {
 public:
  typedef void (*LogFn)(const char* msg);

  void setLogger(LogFn f) { log_ = f; }
  void setUnloadWaitMs(uint32_t ms) { unloadWaitMs_ = ms; }

  // 新的连接：清空打印机状态（等待中的退料保留）
  void reset();

  // 一条 MQTT 消息：begin → 若干次 feed → end
  void beginMessage();
  void feed(const char* d, size_t n) { js_.feed(d, n); }
  void endMessage(uint32_t now);

  // 定期调用：检查退料等待是否超时
  void tick(uint32_t now);
  void cancelPending() { pending_ = false; }

  // 取出待执行的动作（取出后清除），reason 是写进日志的原因
  bool takeStart(const char** reason);
  bool takeUnload(const char** reason);

  // 当前了解到的打印机状态（网页显示用）
  const char* gcodeState() const { return gcode_; }  // "" = 还不知道
  int hwSwitch() const { return hw_; }               // -1 不知道，0 无耗材，1 有耗材
  int stage() const { return stg_; }                 // -1 不知道
  int trayNow() const { return trayNow_; }           // -1 不知道，254 外置料盘，255 无（老协议）
  int externalActive() const { return ext_; }        // -1 不知道，1 正在用外置料盘，0 没有
  uint32_t messages() const { return msgCount_; }
  uint32_t badMessages() const { return badCount_; }
  bool unloadPending() const { return pending_; }
  const char* lastEvent() const { return lastEvent_; }

 private:
  struct Msg {
    char command[24];
    char result[12];
    int useAms;     // -1 没出现，0 false，1 true
    bool external;  // ams_mapping / ams_mapping2 里用到了外置料盘
    int target;     // ams_change_filament 的 target，-1 没出现
    int amsId, slotId;  // ams_change_filament 的 ams_id / slot_id，-1 没出现
    bool hasGcode;
    char gcode[12];
    bool hasStg, hasHw, hasTray, hasErr;
    int stg, hw, tray;
    long err;
    int elemAms, elemSlot;  // ams_mapping2 当前元素
    bool hasSnow, snowExt;  // device.extruder.info[].snow：当前料盘
    bool hasExtInfo, extFil;  // device.extruder.info[].info 的 bit1：挤出机里有耗材
  };

  static void onJson(void* ctx, const JsonScanner& js, JsonScanner::Event ev, const char* v,
                     JsonScanner::Kind k);
  void handle(const JsonScanner& js, JsonScanner::Event ev, const char* v, JsonScanner::Kind k);
  void logf(const char* fmt, ...) __attribute__((format(printf, 2, 3)));
  void event(const char* text);
  void requestStart(const char* reason);
  void requestUnload(const char* reason);
  void unloadCommand(uint32_t now, const char* what);
  static bool isActive(const char* s);    // 打印相关的状态（含切片中）
  static bool isPrinting(const char* s);  // PREPARE / RUNNING / PAUSE
  static bool isStarting(const char* s);

  LogFn log_ = nullptr;
  JsonScanner js_;
  Msg m_ = {};

  char gcode_[12] = "";
  int hw_ = -1, stg_ = -1, trayNow_ = -1, ext_ = -1;
  bool newProto_ = false;  // 见过 device.extruder.info：以它为准，不再看 tray_now / hw_switch_state
  int hwRaw_ = -1;         // hw_switch_state 原始值（只用于日志）
  long err_ = 0;
  bool pending_ = false;
  uint32_t pendingSince_ = 0;
  uint32_t unloadWaitMs_ = 120000;

  bool start_ = false, unload_ = false;
  const char* startReason_ = "";
  const char* unloadReason_ = "";

  uint32_t msgCount_ = 0, badCount_ = 0;
  char lastEvent_[48] = "";
};

}  // namespace semi
