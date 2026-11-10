// 电脑上跑的仿真测试：只测 jsonmac.h（解析设备的 state JSON、mac 规范化），不需要开发板
//   用法：bash host_test/run.sh
#include <stdio.h>
#include <string.h>

#include "../src/jsonmac.h"
#include "../src/topic.h"

static int gFail = 0;

static void check(bool cond, const char* what) {
  if (cond) {
    printf("  ok   %s\n", what);
  } else {
    printf("  FAIL %s\n", what);
    ++gFail;
  }
}

static bool eq(const char* a, const char* b) { return strcmp(a, b) == 0; }

int main() {
  const char* state =
      "{\"state\":\"打印中\",\"key\":\"PRINTING\",\"motor\":\"正转\",\"duty\":60,\"in\":1,"
      "\"ready\":1,\"lack\":0,\"full\":1,\"rssi\":-77,\"heap\":12480,\"uptime\":126,"
      "\"ip\":\"192.168.5.222\",\"mac\":\"84:f3:eb:dd:27:00\"}";

  printf("json::get 字符串\n");
  char buf[64];
  check(semi::json::get(state, "key", buf, sizeof(buf)) && eq(buf, "PRINTING"), "key = PRINTING");
  check(semi::json::get(state, "state", buf, sizeof(buf)) && eq(buf, "打印中"), "state = 打印中");
  check(semi::json::get(state, "ip", buf, sizeof(buf)) && eq(buf, "192.168.5.222"), "ip 取到");
  check(semi::json::get(state, "mac", buf, sizeof(buf)) && eq(buf, "84:f3:eb:dd:27:00"), "mac 取到");
  check(!semi::json::get(state, "nope", buf, sizeof(buf)), "没有的键返回 false");

  printf("json::getLong\n");
  long v = 0;
  check(semi::json::getLong(state, "rssi", &v) && v == -77, "rssi = -77");
  check(semi::json::getLong(state, "duty", &v) && v == 60, "duty = 60（数字不是字符串）");
  check(semi::json::getLong(state, "uptime", &v) && v == 126, "uptime = 126");
  check(!semi::json::getLong(state, "nope", &v), "没有的键返回 false");

  printf("json 边界\n");
  check(semi::json::get("{\"a\":\"x\\\"y\"}", "a", buf, sizeof(buf)) && eq(buf, "x\"y"), "转义引号");
  check(semi::json::get("{\"statex\":\"A\",\"state\":\"B\"}", "state", buf, sizeof(buf)) && eq(buf, "B"),
        "键名有共同前缀时不取错");
  check(semi::json::get("{\"n\":{\"x\":1}}", "n", buf, sizeof(buf)) == false, "嵌套对象不取");
  check(semi::json::get("{\"e\":\"\"}", "e", buf, sizeof(buf)) && eq(buf, ""), "空字符串");
  char small[4];
  check(semi::json::get("{\"s\":\"abcdef\"}", "s", small, sizeof(small)) && eq(small, "abc"),
        "缓冲区不够时截断不越界");

  printf("availability\n");
  const char* av = "{\"state\":\"online\",\"ip\":\"192.168.5.222\",\"mac\":\"84:F3:EB:DD:27:00\"}";
  check(semi::json::get(av, "state", buf, sizeof(buf)) && eq(buf, "online"), "state = online");

  printf("mac 规范化\n");
  char id[13], mac[18];
  check(semi::mac::toId("84:f3:eb:dd:27:00", id, sizeof(id)) && eq(id, "84f3ebdd2700"), "带冒号");
  check(semi::mac::toId("84F3EBDD2700", id, sizeof(id)) && eq(id, "84f3ebdd2700"), "大写无分隔");
  check(semi::mac::toId("84-F3-EB-DD-27-00", id, sizeof(id)) && eq(id, "84f3ebdd2700"), "横线");
  check(semi::mac::normalize("84F3EBDD2700", mac, sizeof(mac)) && eq(mac, "84:f3:eb:dd:27:00"),
        "normalize 统一成小写带冒号");
  check(!semi::mac::toId("84:f3:eb:dd:27", id, sizeof(id)), "位数不够返回 false");
  check(!semi::mac::toId("84:f3:eb:dd:27:0g", id, sizeof(id)), "非十六进制返回 false");
  check(!semi::mac::normalize("84f3ebdd2700", mac, 10), "输出缓冲太小返回 false");

  printf("json 转义\n");
  char out[64];
  semi::json::escapeTo("PLA\"x\\", out, sizeof(out));
  check(eq(out, "PLA\\\"x\\\\"), "引号和反斜杠转义");


  printf("主题过滤器匹配\n");
  check(semi::topic::matches("semiams/+/state", "semiams/84f3ebdd2700/state"), "单层通配符 +");
  check(!semi::topic::matches("semiams/+/state", "semiams/84f3ebdd2700/availability"), "后缀不同不匹配");
  check(!semi::topic::matches("semiams/+/state", "semiams/a/b/state"), "+ 只匹配一层");
  check(semi::topic::matches("semiams/#", "semiams/84f3ebdd2700/state"), "# 匹配多层");
  check(semi::topic::matches("semiams/#", "semiams"), "a/# 也匹配 a");
  check(semi::topic::matches("#", "任意/主题/都行"), "单独的 # 匹配全部");
  check(semi::topic::matches("semiams/84f3ebdd2700/state", "semiams/84f3ebdd2700/state"), "完全相等");
  check(!semi::topic::matches("semiams/control", "semiams/84f3ebdd2700/state"), "control 不匹配 state");
  check(semi::topic::matches("+/control", "semiams/control"), "开头 +");
  check(!semi::topic::matches("semiams/+/state", "semiams//state/"), "结尾多一层不匹配");

  printf("\n%s（失败 %d 项）\n", gFail ? "有测试没通过" : "全部通过", gFail);
  return gFail ? 1 : 0;
}
