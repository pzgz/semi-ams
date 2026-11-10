// MQTT 主题过滤器匹配（纯 C++，可在电脑上测，见 host_test/）
// 规则按 MQTT 3.1.1 第 4.7 节：+ 匹配一层，# 匹配剩余所有层（只能是最后一层）
#pragma once
#include <stddef.h>

namespace semi {
namespace topic {

inline bool matches(const char* filter, const char* name) {
  if (!filter || !name) return false;
  const char* f = filter;
  const char* t = name;
  while (*f) {
    if (*f == '#') return true;  // '#' 之后不再看，等于匹配剩下的全部
    if (*f == '+') {
      ++f;
      if (*f == '/') ++f;
      while (*t && *t != '/') ++t;  // 吃掉主题的一层
      if (*t == '/') ++t;
      if (!*f) return !*t;
      continue;
    }
    // "a/#" 也匹配 "a"（# 包含父层）
    if (*f == '/' && f[1] == '#' && !*t) return true;
    if (*t != *f) return false;
    ++f;
    ++t;
  }
  return *t == 0;
}

}  // namespace topic
}  // namespace semi
