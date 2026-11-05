#include "crash.h"

#include <Arduino.h>
#include <string.h>
#include <user_interface.h>

#include "app.h"

namespace {

constexpr uint32_t kMagic = 0x53414D31;  // "SAM1"
// RTC 用户内存从第 64 块（每块 4 字节）开始；内核的 eboot 升级命令用第 128 块起，这里只用 64~93
constexpr uint8_t kRtcBlock = 64;
constexpr int kAddrN = 16;

// 内核在 postmortem 里用的两个变量：最后一次申请内存失败的调用处和大小
struct Record {
  uint32_t magic;
  uint32_t reason;    // rst_info.reason；254 = panic/abort/assert/内存不足，253 = 栈被破坏
  uint32_t exccause;  // CPU 异常号
  uint32_t epc1;      // 出错的指令地址
  uint32_t excvaddr;  // 出错时访问的内存地址
  uint32_t sys;       // 1 = 崩在系统任务（定时任务、网络回调），0 = loop()
  uint32_t failAddr, failSize;
  uint32_t heapMin;
  uint32_t upSec;
  uint32_t n;
  uint32_t addr[kAddrN];  // 栈上找到的代码地址（从最近的调用开始）
};

volatile uint32_t gHeapMin = 0xFFFFFFFF;

bool isCode(uint32_t a) {
  return (a >= 0x40200000UL && a < 0x40300000UL) || (a >= 0x40100000UL && a < 0x40108000UL);
}

const char* excName(uint32_t c) {
  switch (c) {
    case 0: return PSTR("非法指令");
    case 2: return PSTR("取指令出错");
    case 3: return PSTR("读写出错");
    case 6: return PSTR("除以零");
    case 9: return PSTR("地址未对齐");
    case 28: return PSTR("读非法地址");
    case 29: return PSTR("写非法地址");
    default: return "";
  }
}

}  // namespace

extern void* umm_last_fail_alloc_addr;
extern int umm_last_fail_alloc_size;

// 内核崩溃时调用（弱符号覆盖）。这时中断已关，只做简单的内存读写
extern "C" void custom_crash_callback(struct rst_info* ri, uint32_t stack, uint32_t stackEnd) {
  Record r;
  memset(&r, 0, sizeof(r));
  r.magic = kMagic;
  r.reason = ri->reason;
  r.exccause = ri->exccause;
  r.epc1 = ri->epc1;
  r.excvaddr = ri->excvaddr;
  r.sys = stackEnd == 0x3fffffb0UL ? 1 : 0;
  r.failAddr = (uint32_t)(uintptr_t)umm_last_fail_alloc_addr;
  r.failSize = (uint32_t)umm_last_fail_alloc_size;
  r.heapMin = gHeapMin;
  r.upSec = millis() / 1000;
  for (uint32_t p = stack & ~3UL; p + 4 <= stackEnd && r.n < (uint32_t)kAddrN; p += 4) {
    const uint32_t v = *(const uint32_t*)(uintptr_t)p;
    if (!isCode(v)) continue;
    bool dup = false;
    for (uint32_t i = 0; i < r.n; i++) dup = dup || r.addr[i] == v;
    if (!dup) r.addr[r.n++] = v;
  }
  system_rtc_mem_write(kRtcBlock, &r, sizeof(r));
}

namespace crash {

void noteHeap(uint32_t freeHeap) {
  if (freeHeap < gHeapMin) gHeapMin = freeHeap;
}

uint32_t heapMin() { return gHeapMin; }

void reportLast() {
  Record r;
  if (!system_rtc_mem_read(kRtcBlock, &r, sizeof(r)) || r.magic != kMagic) return;
  const uint32_t zero = 0;
  system_rtc_mem_write(kRtcBlock, &zero, sizeof(zero));
  if (r.n > (uint32_t)kAddrN) r.n = kAddrN;

  const char* ctx = r.sys ? PSTR("系统任务") : PSTR("loop");  // 在 Flash 里，用 %S 打印
  if (r.reason == REASON_EXCEPTION_RST) {
    LOG("上次崩溃：异常 %lu " SEMI_PS " PC=%08lX 地址=%08lX（" SEMI_PS "，%lu秒）", (unsigned long)r.exccause,
             excName(r.exccause), (unsigned long)r.epc1, (unsigned long)r.excvaddr, ctx,
             (unsigned long)r.upSec);
  } else if (r.reason == REASON_SOFT_WDT_RST) {
    LOG("上次崩溃：软件看门狗超时，PC %08lX（" SEMI_PS "，运行 %lu 秒）", (unsigned long)r.epc1, ctx,
             (unsigned long)r.upSec);
  } else if (r.reason == REASON_WDT_RST) {
    LOG("上次崩溃：硬件看门狗超时（运行 %lu 秒）", (unsigned long)r.upSec);
  } else if (r.reason == 254) {
    LOG("上次崩溃：程序中止（panic/abort/assert 或内存不足）（" SEMI_PS "，运行 %lu 秒）", ctx,
             (unsigned long)r.upSec);
  } else if (r.reason == 253) {
    LOG("上次崩溃：栈被破坏（" SEMI_PS "，运行 %lu 秒）", ctx, (unsigned long)r.upSec);
  } else {
    LOG("上次崩溃：原因 %lu（" SEMI_PS "，运行 %lu 秒）", (unsigned long)r.reason, ctx, (unsigned long)r.upSec);
  }
  if (r.failAddr)
    LOG("崩溃前申请内存失败：%lu 字节，调用处 %08lX；开机后最低空闲内存 %lu", (unsigned long)r.failSize,
             (unsigned long)r.failAddr, (unsigned long)r.heapMin);
  else
    LOG("开机后最低空闲内存 %lu", (unsigned long)r.heapMin);

  // 调用栈每行 8 个地址
  for (uint32_t i = 0; i < r.n; i += 8) {
    char line[96];
    size_t len = snprintf_P(line, sizeof(line), PSTR("崩溃调用栈%lu："), (unsigned long)(i / 8 + 1));
    for (uint32_t k = i; k < r.n && k < i + 8 && len < sizeof(line) - 10; k++)
      len += snprintf(line + len, sizeof(line) - len, " %08lX", (unsigned long)r.addr[k]);
    LOG("%s", line);
  }
}

}  // namespace crash
