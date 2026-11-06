// 崩溃记录：程序崩溃（异常、看门狗、panic/abort、内存不足）时，把原因、出错地址和一段调用栈存进
// RTC 内存（软件重启后不丢），下次开机写进日志。不接串口线也能知道上次崩在哪里。
// 解码：xtensa-lx106-elf-addr2line -pfiaC -e .pio/build/esp12s/firmware.elf 地址...
#pragma once
#include <stdint.h>

namespace crash {

// 开机时调用一次：上次是崩溃重启的话，把记录写进日志，然后清除
void reportLast();

// loop() 里调用：记录开机以来的最低空闲内存（崩溃时一起保存）
void noteHeap(uint32_t freeHeap);
uint32_t heapMin();

}  // namespace crash
