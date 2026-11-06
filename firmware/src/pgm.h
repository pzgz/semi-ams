// 字符串常量放进 Flash：ESP8266 上用 PSTR（不占内存），电脑上的仿真测试里原样使用
#pragma once
#include <stdarg.h>
#include <stdio.h>

#if defined(ARDUINO)
#include <pgmspace.h>
#define SEMI_P(s) PSTR(s)
#define SEMI_VSNPRINTF vsnprintf_P
#else
#define SEMI_P(s) (s)
#define SEMI_VSNPRINTF vsnprintf
#endif

// 用 printf 打印"可能在 Flash 里"的字符串（状态名、原因、备注等）：ESP8266 上是 %S，
// 它按 Flash 的方式逐字读取，字符串在内存里也能正确打印。格式里这样写："状态：" SEMI_PS
// 注意：Flash 里的字符串不能直接逐字节读（*p、strcpy、String +=），要用 _P 版本的函数或 FPSTR()
#if defined(ARDUINO)
#define SEMI_PS "%S"
#else
#define SEMI_PS "%s"
#endif

// 复制可能在 Flash 里的字符串（ESP8266 上用 strncpy_P 按 Flash 的方式读），结果一定以 0 结尾
#include <string.h>
inline void semi_copy_P(char* dst, const char* src, size_t cap) {
  if (!cap) return;
#if defined(ARDUINO)
  strncpy_P(dst, src, cap - 1);
#else
  strncpy(dst, src, cap - 1);
#endif
  dst[cap - 1] = 0;
}
