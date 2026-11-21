// 打印机状态：由 PC 端桥接程序通过 MQTT 推过来，这里只负责收和存。
//
// C3 自己不连打印机、不做 TLS，报文是一条扁平 JSON：
//   {"st":"RUNNING","pct":69,"ly":26,"tly":94,"rem":22,"f":"零件","nz":245,...,
//    "ams":"PLA:C52C18:-1|PETG:FFFFFF:-1","vt":"PETG:F72323"}
//
// ams 故意用 "材料:颜色:余量|…" 这种字符串而不是数组：jsonmac 只认扁平键，
// 拆字符串比给固件加个数组解析器省事得多，报文也短。
#pragma once
#include <Arduino.h>

namespace ctrl {

struct PrinterTray {
  char type[8];   // PLA / PETG
  char color[8];  // "C52C18"
  int remain;     // -1 = 未知（非 RFID 卷固件恒报 100，不可信）
};

struct PrinterState {
  bool valid;       // 收到过至少一条报文
  uint32_t lastMs;  // 最后一条报文的 millis
  uint32_t reports; // 累计条数

  // 设备身份（顶栏那行）：型号来自 info.get_version，SN 来自打印机自签证书的
  // CN，都由 PC 端填好再推过来
  char model[20];
  char sn[20];
  char src[8];      // "local" / "cloud"

  char st[12];      // gcode_state：RUNNING / PAUSE / FINISH / FAILED …
  int pct;          // 进度 %
  int layer, total; // 层
  int remainMin;    // 剩余分钟
  char file[48];    // 文件名（UTF-8）

  int nozzle, nozzleTarget;
  int bed, bedTarget;
  int chamber;
  int wifi;         // dBm
  int spd;          // 速度档
  int err;          // print_error

  int hmsCount;
  char hms1[24];    // 四段式，如 "0500-0500-0001-0007"（19 字符 + 结尾 0）

  char amsId[4];    // AMS 单元号
  int amsHum;       // 湿度档位 1-5（固件给的是档位，不是百分比）；-1 = 不报
  int amsTemp;      // 腔温 °C；A1 mini / AMS Lite 不报，-1
  int trayNow;      // 正在取料的槽位：255 空闲 / 254 外挂 / 0-3

  int trayCount;
  PrinterTray tray[4];
  bool hasVt;       // 外挂料盘
  char vtType[8];
  char vtColor[8];
};

// MQTT 报文入口（broker 收到 .../printer 时调用）
void printerOnState(const char* payload);

// 拷一份出来（内部加临界区，网页任务和 broker 任务并发安全）
void printerCopy(PrinterState& out);

// 多久没收到报文了（毫秒）；从没收到过返回 0xFFFFFFFF
uint32_t printerAge();

}  // namespace ctrl
