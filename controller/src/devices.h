// 设备表：来一台建一个槽位，行数跟着设备数走（不再固定 8 行）。
// 哪台落在哪一行看它第一次发消息 / 手动添加的顺序，绑定关系、颜色、材料类型都存进 NVS。
#pragma once
#include <Arduino.h>

#include "config.h"

namespace ctrl {

struct Device {
  char mac[18];      // "84:f3:eb:dd:27:00"，空 = 这个槽位还没有设备
  char ip[16];
  char state[28];    // 状态中文名，例如 打印中
  char key[24];      // 状态英文名，例如 PRINTING
  char motor[10];    // 停止 / 正转 / 反转
  int duty;
  int in;            // 进料口开关
  int ready;         // 就绪开关
  int lack;          // EMPTY 霍尔
  int full;          // FULL 霍尔
  long rssi;
  long heap;
  long uptime;
  bool online;
  uint32_t lastSeenMs;  // 最后一条消息的 millis（0 = 从没收到）
  char color[8];        // "#3b6fd4"
  char material[16];    // PLA / PETG / …
};

void devicesBegin();
Device& deviceAt(int i);  // 调用前先用 devicesUsed() 判范围
int devicesUsed();        // 当前表里有多少台（= 界面上的行数）
int devicesOnline();      // 在线台数

// 收到 MQTT 消息
void devicesOnState(const char* payload);
void devicesOnAvailability(const char* payload);

// 定时调用：把超时没消息的设备标成离线
void devicesLoop();

// 颜色 / 材料类型（网页上改的），存 NVS
bool devicesSetLabel(int idx, const char* color, const char* material, String& err);

// 手动添加一台设备（网页上的「添加设备」）：mac 必填，颜色 / 材料可选
// 成功时 idxOut 返回落在第几行（0 起）
bool devicesAdd(const char* mac, const char* color, const char* material, int& idxOut, String& err);

// 删掉一行（网页上的「删除」按钮）：后面的行往前补一位，NVS 按新序号重写。
// 注意：只删表里的记录，设备下次上报消息还会被重新加到最后一行。
bool devicesRemove(int idx, String& err);

// 12 位设备 ID（mac 去掉冒号）→ 槽位序号，找不到返回 -1
int devicesFindByMacId(const char* macId);
}  // namespace ctrl
