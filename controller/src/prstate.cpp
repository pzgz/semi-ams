#include "prstate.h"

#include <string.h>

#include "jsonmac.h"

namespace ctrl {
namespace {

PrinterState gP;
portMUX_TYPE gMux = portMUX_INITIALIZER_UNLOCKED;

void copyStr(char* dst, size_t cap, const char* src) {
  if (!src) return;
  size_t n = strlen(src);
  // UTF-8 边界回退：宁可少一个汉字，也不要把多字节字符切一半（网页 JSON.parse 会炸）
  if (n >= cap) {
    n = cap - 1;
    while (n > 0 && ((unsigned char)src[n] & 0xC0) == 0x80) --n;
  }
  memcpy(dst, src, n);
  dst[n] = 0;
}

long num(const char* js, const char* key, long def = 0) {
  long v = 0;
  return semi::json::getLong(js, key, &v) ? v : def;
}

// "PLA:C52C18:-1|PETG:FFFFFF:-1" → tray[]
int parseTrays(const char* s, PrinterTray* out, int max) {
  int n = 0;
  while (s && *s && n < max) {
    const char* bar = strchr(s, '|');
    const size_t len = bar ? (size_t)(bar - s) : strlen(s);
    if (len && len < 24) {
      char seg[24];
      memcpy(seg, s, len);
      seg[len] = 0;
      char* c1 = strchr(seg, ':');
      if (c1) {
        *c1++ = 0;
        char* c2 = strchr(c1, ':');
        if (c2) *c2++ = 0;
        if (seg[0]) {
          copyStr(out[n].type, sizeof(out[n].type), seg);
          copyStr(out[n].color, sizeof(out[n].color), c1);
          out[n].remain = c2 ? (int)strtol(c2, nullptr, 10) : -1;
          ++n;
        }
      }
    }
    if (!bar) break;
    s = bar + 1;
  }
  return n;
}

}  // namespace

void printerOnState(const char* js) {
  if (!js || !js[0]) return;

  PrinterState s;
  memset(&s, 0, sizeof(s));
  char buf[64];

  if (semi::json::get(js, "st", buf, sizeof(buf))) copyStr(s.st, sizeof(s.st), buf);
  // 身份字段：PC 端每条都带，缺了留空即可（顶栏自己会藏空段）
  char idbuf[48];
  if (semi::json::get(js, "md", idbuf, sizeof(idbuf))) copyStr(s.model, sizeof(s.model), idbuf);
  if (semi::json::get(js, "sn", idbuf, sizeof(idbuf))) copyStr(s.sn, sizeof(s.sn), idbuf);
  if (semi::json::get(js, "src", idbuf, sizeof(idbuf))) copyStr(s.src, sizeof(s.src), idbuf);
  s.pct = (int)num(js, "pct");
  s.layer = (int)num(js, "ly");
  s.total = (int)num(js, "tly");
  s.remainMin = (int)num(js, "rem");
  if (semi::json::get(js, "f", buf, sizeof(buf))) copyStr(s.file, sizeof(s.file), buf);
  s.nozzle = (int)num(js, "nz");
  s.nozzleTarget = (int)num(js, "nzt");
  s.bed = (int)num(js, "bd");
  s.bedTarget = (int)num(js, "bdt");
  s.chamber = (int)num(js, "ch");
  s.wifi = (int)num(js, "wf");
  s.spd = (int)num(js, "spd");
  s.err = (int)num(js, "er");
  s.hmsCount = (int)num(js, "hn");
  if (semi::json::get(js, "h1", buf, sizeof(buf))) copyStr(s.hms1, sizeof(s.hms1), buf);

  s.amsHum = (int)num(js, "amsh", -1);
  s.amsTemp = (int)num(js, "amst", -1);
  s.trayNow = (int)num(js, "tn", 255);   // 缺字段就当空闲，别把 0 当成槽位1
  if (semi::json::get(js, "aid", buf, sizeof(buf))) copyStr(s.amsId, sizeof(s.amsId), buf);

  char ams[200];
  if (semi::json::get(js, "ams", ams, sizeof(ams))) {
    s.trayCount = parseTrays(ams, s.tray, 4);
  }
  char vt[32];
  if (semi::json::get(js, "vt", vt, sizeof(vt))) {
    const char* c = strchr(vt, ':');
    if (c) {
      // 就地拆 "PETG:F72323"
      const size_t tl = (size_t)(c - vt);
      if (tl < sizeof(s.vtType)) {
        memcpy(s.vtType, vt, tl);
        s.vtType[tl] = 0;
        copyStr(s.vtColor, sizeof(s.vtColor), c + 1);
        s.hasVt = true;
      }
    }
  }

  s.valid = true;
  s.lastMs = millis();

  portENTER_CRITICAL(&gMux);
  s.reports = gP.reports + 1;
  gP = s;
  portEXIT_CRITICAL(&gMux);
}

void printerCopy(PrinterState& out) {
  portENTER_CRITICAL(&gMux);
  out = gP;
  out.reports = gP.reports;
  portEXIT_CRITICAL(&gMux);
}

uint32_t printerAge() {
  portENTER_CRITICAL(&gMux);
  const bool valid = gP.valid;
  const uint32_t last = gP.lastMs;
  portEXIT_CRITICAL(&gMux);
  return valid ? (millis() - last) : 0xFFFFFFFFu;
}

}  // namespace ctrl
