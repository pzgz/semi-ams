// 流式 JSON 扫描器（纯 C++，可在电脑上测试）
//
// 拓竹打印机的状态消息可能有十几 KB，ESP8266 放不下整条消息。这里逐字节扫描，
// 只在遇到值、对象/数组开始和结束时回调，并给出从根开始的键路径：
//   {"print":{"command":"project_file","ams_mapping2":[{"ams_id":254}]}}
//   "project_file" 的路径是 print / command
//   254            的路径是 print / ams_mapping2 / [] / ams_id   （数组元素记为 "[]"）
// 内存占用固定（约 300 字节），消息多长都可以。
#pragma once
#include <stdint.h>
#include <string.h>

namespace semi {

class JsonScanner {
 public:
  static const int kMaxDepth = 8;   // 只记录前 8 层的键名，更深的层仍能正确跳过
  static const int kKeyLen = 24;    // 键名最多保留 23 个字符
  static const int kValLen = 48;    // 值最多保留 47 个字符（超出部分丢弃，truncated() 为 true）

  enum class Event : uint8_t { Value, ObjectBegin, ObjectEnd, ArrayBegin, ArrayEnd };
  enum class Kind : uint8_t { String, Number, True, False, Null, None };

  // 回调：ev = Value 时 value/kind 有效；对象/数组的开始和结束，path() 是这个容器所在的位置
  typedef void (*Handler)(void* ctx, const JsonScanner& js, Event ev, const char* value, Kind kind);

  void setHandler(Handler h, void* ctx) {
    handler_ = h;
    ctx_ = ctx;
  }

  void reset() {
    st_ = St::Value;
    depth_ = 0;
    len_ = 0;
    truncated_ = false;
    escape_ = false;
    uniLeft_ = 0;
    error_ = false;
    done_ = false;
    first_ = true;
    deepTypes_ = 0;
    deepIsObj_ = false;
  }

  // 输入一个字节；出错后忽略后续内容
  void feed(char c) {
    if (error_) return;
    switch (st_) {
      case St::Value: valueStart(c); break;
      case St::Key: keyStart(c); break;
      case St::Colon:
        if (isSpace(c)) return;
        if (c == ':') st_ = St::Value;
        else fail();
        break;
      case St::After: afterValue(c); break;
      case St::Str: strChar(c); break;
      case St::Lit: litChar(c); break;
    }
  }

  void feed(const char* s, size_t n) {
    for (size_t i = 0; i < n; i++) feed(s[i]);
  }

  bool error() const { return error_; }
  bool done() const { return done_; }  // 根对象/数组已完整结束
  bool truncated() const { return truncated_; }

  // 当前路径（不含正在处理的值本身）
  int depth() const { return depth_; }
  const char* key(int i) const { return (i >= 0 && i < kMaxDepth && i < depth_) ? keys_[i] : ""; }

  // 路径是否正好等于给定的键（最多 6 层）
  bool pathIs(const char* a, const char* b = nullptr, const char* c = nullptr, const char* d = nullptr,
              const char* e = nullptr, const char* f = nullptr) const {
    const char* p[6] = {a, b, c, d, e, f};
    int n = 0;
    while (n < 6 && p[n]) n++;
    if (n != depth_ || n > kMaxDepth) return false;
    for (int i = 0; i < n; i++)
      if (strcmp(keys_[i], p[i]) != 0) return false;
    return true;
  }

 private:
  enum class St : uint8_t { Value, Key, Colon, After, Str, Lit };

  static bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

  void fail() { error_ = true; }

  void emit(Event ev, const char* v, Kind k) {
    if (handler_) handler_(ctx_, *this, ev, v, k);
  }

  bool inObject() const { return depth_ > 0 && (depth_ > kMaxDepth ? deepIsObj_ : isObj_[depth_ - 1]); }

  void push(bool obj) {
    emit(obj ? Event::ObjectBegin : Event::ArrayBegin, "", Kind::None);
    if (depth_ < kMaxDepth) {
      isObj_[depth_] = obj;
      keys_[depth_][0] = 0;
      if (!obj) strcpy(keys_[depth_], "[]");
    } else {
      deepIsObj_ = obj;  // 超过记录深度：只记最内层类型（足够判断逗号/冒号）
      deepTypes_ = (deepTypes_ << 1) | (obj ? 1 : 0);
    }
    depth_++;
    st_ = obj ? St::Key : St::Value;
    first_ = true;
  }

  void pop(bool obj) {
    if (depth_ == 0 || inObject() != obj) return fail();
    depth_--;
    if (depth_ >= kMaxDepth) {
      deepTypes_ >>= 1;
      deepIsObj_ = depth_ > kMaxDepth ? (deepTypes_ & 1) : false;
    }
    emit(obj ? Event::ObjectEnd : Event::ArrayEnd, "", Kind::None);
    if (depth_ == 0) done_ = true;
    st_ = St::After;
  }

  void valueStart(char c) {
    if (isSpace(c)) return;
    if (depth_ == 0 && done_) return fail();  // 根之后又出现内容
    if (c == '{') return push(true);
    if (c == '[') {
      push(false);
      return;
    }
    if (c == ']' && !inObject() && first_) {  // 空数组
      pop(false);
      return;
    }
    if (c == '"') {
      startStr(false);
      return;
    }
    if (c == '-' || (c >= '0' && c <= '9') || c == 't' || c == 'f' || c == 'n') {
      len_ = 0;
      truncated_ = false;
      add(c);
      st_ = St::Lit;
      return;
    }
    fail();
  }

  void keyStart(char c) {
    if (isSpace(c)) return;
    if (c == '}' && first_) return pop(true);  // 空对象
    if (c == '"') return startStr(true);
    fail();
  }

  void afterValue(char c) {
    if (isSpace(c)) return;
    if (depth_ == 0) return fail();
    if (c == ',') {
      first_ = false;
      st_ = inObject() ? St::Key : St::Value;
      return;
    }
    if (c == '}') return pop(true);
    if (c == ']') return pop(false);
    fail();
  }

  void startStr(bool isKey) {
    strIsKey_ = isKey;
    len_ = 0;
    truncated_ = false;
    escape_ = false;
    uniLeft_ = 0;
    st_ = St::Str;
  }

  void add(char c) {
    if (len_ < kValLen - 1) buf_[len_++] = c;
    else truncated_ = true;
  }

  void strChar(char c) {
    if (uniLeft_) {  // \uXXXX：跳过 4 位十六进制，用 '?' 代替
      if (--uniLeft_ == 0) add('?');
      return;
    }
    if (escape_) {
      escape_ = false;
      switch (c) {
        case 'n': add('\n'); break;
        case 't': add('\t'); break;
        case 'r': add('\r'); break;
        case 'b': add('\b'); break;
        case 'f': add('\f'); break;
        case 'u': uniLeft_ = 4; break;
        default: add(c); break;  // \" \\ \/
      }
      return;
    }
    if (c == '\\') {
      escape_ = true;
      return;
    }
    if (c != '"') {
      add(c);
      return;
    }
    buf_[len_] = 0;
    if (strIsKey_) {
      if (depth_ <= kMaxDepth) {
        strncpy(keys_[depth_ - 1], buf_, kKeyLen - 1);
        keys_[depth_ - 1][kKeyLen - 1] = 0;
      }
      st_ = St::Colon;
    } else {
      emit(Event::Value, buf_, Kind::String);
      st_ = St::After;
    }
  }

  void litChar(char c) {
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '+' || c == 'E') {
      add(c);
      return;
    }
    buf_[len_] = 0;
    Kind k = Kind::Number;
    if (!strcmp(buf_, "true")) k = Kind::True;
    else if (!strcmp(buf_, "false")) k = Kind::False;
    else if (!strcmp(buf_, "null")) k = Kind::Null;
    else if (buf_[0] == 't' || buf_[0] == 'f' || buf_[0] == 'n') return fail();
    emit(Event::Value, buf_, k);
    st_ = St::After;
    afterValue(c);  // 这个字符是数字后面的分隔符
  }

  Handler handler_ = nullptr;
  void* ctx_ = nullptr;
  St st_ = St::Value;
  int depth_ = 0;
  bool isObj_[kMaxDepth] = {};
  char keys_[kMaxDepth][kKeyLen] = {};
  bool deepIsObj_ = false;
  uint32_t deepTypes_ = 0;
  char buf_[kValLen] = {};
  int len_ = 0;
  bool truncated_ = false;
  bool strIsKey_ = false;
  bool escape_ = false;
  uint8_t uniLeft_ = 0;
  bool first_ = true;
  bool error_ = false;
  bool done_ = false;
};

}  // namespace semi
