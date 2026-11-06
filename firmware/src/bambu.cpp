#include "bambu.h"

#include "pgm.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace semi {

namespace {

void copy(char* dst, size_t cap, const char* src) {
  strncpy(dst, src, cap - 1);
  dst[cap - 1] = 0;
}

bool startsWithNoCase(const char* s, const char* prefix) {
  for (; *prefix; s++, prefix++) {
    char a = *s, b = *prefix;
    if (a >= 'A' && a <= 'Z') a += 32;
    if (b >= 'A' && b <= 'Z') b += 32;
    if (a != b) return false;
  }
  return true;
}

const char* hwText(int v) { return v < 0 ? SEMI_P("未知") : (v ? SEMI_P("有耗材") : SEMI_P("无耗材")); }

// 外置料盘退料的最后一步：请从挤出机拔出外置料盘的耗材（07FE 是 H2 系列的另一个外置料盘）。
// 0x07FFC003 一直没人拔，约 2 分钟后打印机换成 0x07FF8003（HMS 07FF-8003，意思相同）
bool isPullOutPrompt(uint32_t e) {
  return e == 0x07FFC003UL || e == 0x07FEC003UL || e == 0x07FF8003UL || e == 0x07FE8003UL;
}

const char* errText(uint32_t e) {
  if (e == 0x0300400CUL) return SEMI_P("（打印任务已取消）");
  if (isPullOutPrompt(e)) return SEMI_P("（请从挤出机拔出外置料盘的耗材）");
  return "";
}

}  // namespace

void BambuMonitor::reset() {
  gcode_[0] = 0;
  hw_ = stg_ = trayNow_ = ext_ = hwRaw_ = -1;
  newProto_ = false;
  err_ = 0;
  // pending_ 保留：退料等待中途断线重连，重连后继续等
}

void BambuMonitor::beginMessage() {
  memset(&m_, 0, sizeof(m_));
  m_.useAms = -1;
  m_.target = -1;
  m_.amsId = m_.slotId = -1;
  m_.elemAms = m_.elemSlot = -1;
  js_.reset();
  js_.setHandler(&BambuMonitor::onJson, this);
}

void BambuMonitor::onJson(void* ctx, const JsonScanner& js, JsonScanner::Event ev, const char* v,
                          JsonScanner::Kind k) {
  static_cast<BambuMonitor*>(ctx)->handle(js, ev, v, k);
}

void BambuMonitor::handle(const JsonScanner& js, JsonScanner::Event ev, const char* v, JsonScanner::Kind k) {
  typedef JsonScanner::Event E;
  typedef JsonScanner::Kind K;

  // ams_mapping2 的每个元素：{"ams_id":254,"slot_id":0}
  if (js.pathIs("print", "ams_mapping2", "[]")) {
    if (ev == E::ObjectBegin) {
      m_.elemAms = m_.elemSlot = -1;
    } else if (ev == E::ObjectEnd) {
      const int a = m_.elemAms, s = m_.elemSlot;
      // 外置料盘：ams_id 254 或 255；{"ams_id":255,"slot_id":255} 表示"未分配"
      if ((a == 254 && s != 255) || (a == 255 && s >= 0 && s != 255)) m_.external = true;
    }
    return;
  }
  if (ev != E::Value) return;
  const bool num = k == K::Number || k == K::String;  // 有些数字字段是字符串，例如 tray_now
  const long n = num ? atol(v) : 0;

  // 新协议打印机的挤出机信息
  if (js.pathIs("print", "device", "extruder", "info", "[]", "snow")) {
    if (k == K::Number) {
      m_.hasSnow = true;
      const long a = (n >> 8) & 0xFF, s = n & 0xFF;
      if ((a == 254 || a == 255) && s != 255) m_.snowExt = true;
    }
    return;
  }
  if (js.pathIs("print", "device", "extruder", "info", "[]", "info")) {
    if (k == K::Number) {
      m_.hasExtInfo = true;
      if (n & 2) m_.extFil = true;
    }
    return;
  }

  if (js.pathIs("print", "ams_mapping2", "[]", "ams_id")) {
    if (num) m_.elemAms = (int)n;
  } else if (js.pathIs("print", "ams_mapping2", "[]", "slot_id")) {
    if (num) m_.elemSlot = (int)n;
  } else if (js.pathIs("print", "ams_mapping", "[]")) {
    if (k == K::Number && n == 254) m_.external = true;  // 旧格式：254 = 外置料盘
  } else if (js.pathIs("print", "command")) {
    copy(m_.command, sizeof(m_.command), v);
  } else if (js.pathIs("print", "result")) {
    copy(m_.result, sizeof(m_.result), v);
  } else if (js.pathIs("print", "use_ams")) {
    if (k == K::True || (k == K::String && !strcmp(v, "true"))) m_.useAms = 1;
    else if (k == K::False || (k == K::String && !strcmp(v, "false"))) m_.useAms = 0;
  } else if (js.pathIs("print", "target")) {
    if (num) m_.target = (int)n;
  } else if (js.pathIs("print", "ams_id")) {
    if (num) m_.amsId = (int)n;
  } else if (js.pathIs("print", "slot_id")) {
    if (num) m_.slotId = (int)n;
  } else if (js.pathIs("print", "gcode_state")) {
    m_.hasGcode = true;
    copy(m_.gcode, sizeof(m_.gcode), v);
  } else if (js.pathIs("print", "stg_cur")) {
    if (num) {
      m_.hasStg = true;
      m_.stg = (int)n;
    }
  } else if (js.pathIs("print", "hw_switch_state")) {
    if (num) {
      m_.hasHw = true;
      m_.hw = (int)n;  // 位掩码：bit0 = 右侧 / 唯一的挤出机
    }
  } else if (js.pathIs("print", "ams", "tray_now")) {
    if (num) {
      m_.hasTray = true;
      m_.tray = (int)n;
    }
  } else if (js.pathIs("print", "print_error")) {
    if (num) {
      m_.hasErr = true;
      m_.err = (uint32_t)strtoul(v, nullptr, 10);
    }
  }
}

void BambuMonitor::endMessage(uint32_t now) {
  if (js_.error() || !js_.done()) {
    badCount_++;
    if (badCount_ == 1 || badCount_ % 50 == 0) logf(SEMI_P("打印机消息格式无法解析（累计 %lu 条）"), (unsigned long)badCount_);
    return;
  }
  msgCount_++;
  now_ = now;

  // ---- 状态字段（先处理料盘，再处理打印状态，备用的开始判断要用到料盘）----
  if (m_.hasSnow || m_.hasExtInfo) newProto_ = true;
  if (m_.hasTray) trayNow_ = m_.tray;
  int ext = -1;
  if (m_.hasSnow) ext = m_.snowExt ? 1 : 0;
  else if (m_.hasTray && !newProto_) ext = m_.tray == 254 ? 1 : 0;
  if (ext >= 0 && ext != ext_) {
    if (ext_ != -1 || ext) logf(SEMI_P("打印机" SEMI_PS "外置料盘"), ext ? SEMI_P("正在用") : SEMI_P("没有用"));
    ext_ = ext;
  }
  if (m_.hasStg && m_.stg != stg_) {
    logf(SEMI_P("打印机阶段：%d → %d"), stg_, m_.stg);
    stg_ = m_.stg;
  }
  if (m_.hasErr && m_.err != err_) {
    if (m_.err) logf(SEMI_P("打印机错误码：0x%08lX" SEMI_PS), (unsigned long)m_.err, errText(m_.err));
    const bool wasPrompt = isPullOutPrompt(err_);
    err_ = m_.err;
    // 打印机已切断耗材、回抽完，正在等人把耗材从挤出机拔出来：现在退料不会和挤出机对拉。
    // 从一种拔出提示换成另一种（C003 → 8003）不重复触发
    if (isPullOutPrompt(err_) && !wasPrompt) {
      pending_ = false;
      requestUnload(SEMI_P("打印机提示拔出外置料盘的耗材"));
    }
  }
  if (m_.hasGcode && strcmp(m_.gcode, gcode_) != 0) {
    const bool known = gcode_[0] != 0;
    const bool wasPrinting = isPrinting(gcode_);
    logf(SEMI_P("打印机状态：%s → %s"), known ? gcode_ : "?", m_.gcode);
    copy(gcode_, sizeof(gcode_), m_.gcode);
    if (known && !wasPrinting && isStarting(gcode_)) {
      pending_ = false;
      if (ext_ == 1) requestStart(SEMI_P("打印机开始打印（外置料盘，按打印状态判断）"));
    }
  }
  // 挤出机里有没有耗材：新协议看 extruder.info 的 bit1，老协议看 hw_switch_state 的 bit0
  int hw = -1;
  int raw = -1;
  if (m_.hasExtInfo) hw = m_.extFil ? 1 : 0;
  else if (m_.hasHw && !newProto_) hw = m_.hw & 1;
  if (m_.hasHw) {
    raw = m_.hw;
    if (newProto_ && raw != hwRaw_) logf(SEMI_P("打印机 hw_switch_state：%d → %d"), hwRaw_, raw);
    hwRaw_ = raw;
  }
  if (hw >= 0 && hw != hw_) {
    const int old = hw_;
    hw_ = hw;
    if (raw >= 0 && !m_.hasExtInfo) logf(SEMI_P("打印机耗材传感器：" SEMI_PS " → " SEMI_PS "（hw_switch_state=%d）"), hwText(old), hwText(hw_), raw);
    else logf(SEMI_P("打印机耗材传感器：" SEMI_PS " → " SEMI_PS), hwText(old), hwText(hw_));
    const bool recentlyUnloaded = everUnload_ && now - lastUnloadAt_ < 180000UL;
    if (old == 1 && hw_ == 0 && !pending_ && !recentlyUnloaded && gcode_[0] && !isActive(gcode_))
      requestUnload(SEMI_P("打印机空闲时耗材离开了打印机"));
  }

  // ---- 命令回报 ----
  if (m_.command[0] && strcmp(m_.command, "push_status") != 0) {
    const bool failed = m_.result[0] && startsWithNoCase(m_.result, "fail");
    if (!strcmp(m_.command, "project_file")) {
      const bool ext = m_.useAms == 0 || m_.external;
      logf(SEMI_P("打印机开始打印（" SEMI_PS "）" SEMI_PS), ext ? SEMI_P("外置料盘") : SEMI_P("AMS，不送料"),
           failed ? SEMI_P("，但命令失败") : "");
      if (ext && !failed) {
        pending_ = false;
        requestStart(SEMI_P("打印机开始打印（外置料盘）"));
      }
    } else if (!strcmp(m_.command, "ams_change_filament")) {
      const int t = m_.target, a = m_.amsId, s = m_.slotId;
      // 退料：target 255，且 slot_id 不存在（老协议）或为 255；ams_id 存在时只认外置料盘（254/255）
      const bool unload = t == 255 && (s < 0 || s == 255) && (a < 0 || a == 254 || a == 255);
      const bool loadExt = t == 254 || (t == 255 && s >= 0 && s != 255) || ((a == 254 || a == 255) && s >= 0 && s != 255);
      if (failed) logf(SEMI_P("打印机换料命令失败（target=%d ams=%d slot=%d）"), t, a, s);
      else if (unload) unloadCommand(now, SEMI_P("打印机退料"));
      else if (loadExt) logf(SEMI_P("打印机进料（外置料盘，target=%d ams=%d slot=%d）"), t, a, s);
      else logf(SEMI_P("打印机换料（target=%d ams=%d slot=%d），不是外置料盘，忽略"), t, a, s);
    } else if (!strcmp(m_.command, "unload_filament")) {
      if (!failed) unloadCommand(now, SEMI_P("打印机退料"));
    } else {
      logf(SEMI_P("打印机命令：%s%s%s"), m_.command, m_.result[0] ? " " : "", m_.result);
    }
  }

  // 等待中的退料：打印机的耗材传感器已经是"无"
  if (pending_ && hw_ == 0) {
    pending_ = false;
    requestUnload(SEMI_P("打印机已退料，耗材已离开打印机"));
  }
}

void BambuMonitor::unloadCommand(uint32_t now, const char* what) {
  if (hw_ == 0 || unloadWaitMs_ == 0) {
    pending_ = false;
    requestUnload(what);
    return;
  }
  pending_ = true;
  pendingSince_ = now;
  logf(SEMI_P(SEMI_PS "：等打印机切断、回抽耗材后再退（最长等 %lu 秒）"), what, (unsigned long)(unloadWaitMs_ / 1000));
  event(SEMI_P("等待打印机退料"));
}

void BambuMonitor::tick(uint32_t now) {
  now_ = now;
  if (pending_ && now - pendingSince_ >= unloadWaitMs_) {
    pending_ = false;
    requestUnload(SEMI_P("打印机退料后等待超时"));
  }
}

bool BambuMonitor::takeStart(const char** reason) {
  if (!start_) return false;
  start_ = false;
  if (reason) *reason = startReason_;
  return true;
}

bool BambuMonitor::takeUnload(const char** reason) {
  if (!unload_) return false;
  unload_ = false;
  if (reason) *reason = unloadReason_;
  return true;
}

void BambuMonitor::requestStart(const char* reason) {
  start_ = true;
  unload_ = false;
  startReason_ = reason;
  event(SEMI_P("开始打印 → 送料"));
}

void BambuMonitor::requestUnload(const char* reason) {
  everUnload_ = true;
  lastUnloadAt_ = now_;
  unload_ = true;
  start_ = false;
  unloadReason_ = reason;
  event(SEMI_P("打印机退料 → 退料"));
}

void BambuMonitor::event(const char* text) { semi_copy_P(lastEvent_, text, sizeof(lastEvent_)); }  // text 在 Flash 里

bool BambuMonitor::isActive(const char* s) {
  return !strcmp(s, "PREPARE") || !strcmp(s, "RUNNING") || !strcmp(s, "PAUSE") || !strcmp(s, "SLICING");
}

bool BambuMonitor::isPrinting(const char* s) {
  return !strcmp(s, "PREPARE") || !strcmp(s, "RUNNING") || !strcmp(s, "PAUSE");
}

bool BambuMonitor::isStarting(const char* s) { return !strcmp(s, "PREPARE") || !strcmp(s, "RUNNING"); }

void BambuMonitor::logf(const char* fmt, ...) {
  if (!log_) return;
  char buf[112];
  va_list ap;
  va_start(ap, fmt);
  SEMI_VSNPRINTF(buf, sizeof(buf), fmt, ap);  // fmt 在 Flash 里（SEMI_P）
  va_end(ap);
  log_(buf);
}

}  // namespace semi
