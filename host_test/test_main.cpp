// 在电脑上运行的仿真测试：g++ -std=c++17 -I../src test_main.cpp ../src/fsm.cpp && ./a.out
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "fsm.h"
#include "input.h"
#include "motor_logic.h"

using namespace semi;

static int g_fail = 0, g_pass = 0;
static std::vector<std::string> g_log;
static bool g_verbose = false;

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (cond) g_pass++;                                                        \
    else {                                                                     \
      g_fail++;                                                                \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
    }                                                                          \
  } while (0)

static void logger(const char* m) {
  g_log.push_back(m);
  if (g_verbose) std::printf("    log: %s\n", m);
}

struct Sim {
  Fsm fsm;
  Inputs in;
  uint32_t now = 1000;
  Params p;
  explicit Sim(Inputs boot = Inputs()) {
    fsm.setParams(p);
    fsm.setLogger(logger);
    in = boot;
    fsm.begin(in, now);
  }
  // 推进 ms 毫秒，每 10ms 调用一次 update；命令只在第一个 tick 生效
  void run(uint32_t ms, Commands c = Commands()) {
    for (uint32_t t = 0; t < ms; t += 10) {
      now += 10;
      fsm.update(in, c, now);
      c = Commands();
    }
  }
  void press(bool startStop, bool emergency = false) {
    Commands c;
    c.startStop = startStop;
    c.emergency = emergency;
    run(10, c);
  }
  State st() const { return fsm.state(); }
  MotorDir m() const { return fsm.motor().dir; }
};

static void title(const char* t) { std::printf("[test] %s\n", t); }

static void test_auto_load() {
  title("插入耗材 -> 延时 -> 吸入 -> 就绪");
  Sim s;
  CHECK(s.st() == State::NoMaterial);
  s.in.in = true;
  s.run(400);
  CHECK(s.st() == State::NoMaterial);  // 还在插入延时内
  CHECK(s.m() == MotorDir::Stop);
  s.run(200);
  CHECK(s.st() == State::Loading);
  CHECK(s.m() == MotorDir::Forward);
  CHECK(s.fsm.motor().speed == s.p.loadSpeed);
  s.in.ready = true;
  s.run(10);
  CHECK(s.st() == State::Ready);
  CHECK(s.m() == MotorDir::Stop);
}

static void test_ready_switch_fallback() {
  title("吸入时就绪开关没触发，满料霍尔触发 -> 判为就绪");
  Sim s;
  s.in.in = true;
  s.run(600);
  CHECK(s.st() == State::Loading);
  s.in.full = true;
  s.run(10);
  CHECK(s.st() == State::Ready);
  CHECK(s.m() == MotorDir::Stop);
}

static void test_load_timeout() {
  title("吸入超时 -> 紧急停止，按红色按钮恢复");
  Sim s;
  s.in.in = true;
  s.run(600);
  s.run(s.p.loadTimeoutMs + 50);
  CHECK(s.st() == State::Emergency);
  CHECK(s.m() == MotorDir::Stop);
  CHECK(std::strcmp(s.fsm.note(), "吸入超时") == 0);
  s.press(false, true);
  CHECK(s.st() == State::Loading);
  CHECK(s.m() == MotorDir::Forward);
}

static void test_user_pulls_filament_out() {
  title("就绪后用户往外抽耗材：不能被电机吸回去");
  Sim s(Inputs{true, true, false, false});
  CHECK(s.st() == State::Ready);
  s.in.ready = false;  // 先离开就绪开关
  s.run(1000);
  CHECK(s.st() == State::Retracted);
  CHECK(s.m() == MotorDir::Stop);
  s.in.in = false;  // 再离开进料口
  s.run(20);
  CHECK(s.st() == State::NoMaterial);
  CHECK(s.m() == MotorDir::Stop);
  s.in.in = true;  // 重新插入才会吸入
  s.run(600);
  CHECK(s.st() == State::Loading);
}

static void test_no_reload_right_after_unload_pullout() {
  title("吸入中耗材被抽出 -> 无耗材，再插入重新吸入");
  Sim s;
  s.in.in = true;
  s.run(600);
  CHECK(s.st() == State::Loading);
  s.in.in = false;
  s.run(10);
  CHECK(s.st() == State::NoMaterial);
  CHECK(s.m() == MotorDir::Stop);
  s.in.in = true;
  s.run(600);
  CHECK(s.st() == State::Loading);
}

static void test_printing_buffer() {
  title("打印：先送到满，空了再补，满了就停");
  Sim s(Inputs{true, true, false, false});
  s.press(true);
  CHECK(s.st() == State::Printing);
  s.run(10);
  CHECK(s.m() == MotorDir::Forward);  // 开始打印先送料到满
  CHECK(s.fsm.motor().speed == s.p.feedSpeed);
  s.run(5000);
  CHECK(s.m() == MotorDir::Forward);
  s.in.full = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Stop);
  // 打印机把缓冲器拉空
  s.in.full = false;
  s.run(3000);
  CHECK(s.m() == MotorDir::Stop);  // 中间位置不补料
  s.in.lack = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Forward);
  s.in.lack = false;
  s.run(2000);
  CHECK(s.m() == MotorDir::Forward);  // 离开 EMPTY 继续补，直到 FULL
  s.in.full = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Stop);
  CHECK(s.st() == State::Printing);
}

static void test_fast_magnet_move() {
  title("磁铁从 EMPTY 快速移到 FULL（原固件会漏检的情况）");
  Sim s(Inputs{true, true, false, true});
  s.press(true);
  s.run(10);
  CHECK(s.m() == MotorDir::Stop);  // 已满，不送料
  // EMPTY 触发，20ms 后释放，紧接着 FULL 触发
  s.in.full = false;
  s.in.lack = true;
  s.run(20);
  CHECK(s.m() == MotorDir::Forward);
  s.in.lack = false;
  s.run(10);
  s.in.full = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Stop);
  // 磁铁在 FULL 旁边晃，不离开，始终保持停止
  for (int i = 0; i < 20; i++) {
    s.run(30);
    CHECK(s.m() == MotorDir::Stop);
  }
  // 反方向同样
  s.in.full = false;
  s.in.lack = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Forward);
}

static void test_feed_timeout_and_resume() {
  title("补料超时 -> 紧急停止 -> 解除后继续打印");
  Sim s(Inputs{true, true, false, true});
  s.press(true);
  s.in.full = false;
  s.in.lack = true;
  s.run(100);
  CHECK(s.m() == MotorDir::Forward);
  s.run(s.p.feedTimeoutMs + 100);
  CHECK(s.st() == State::Emergency);
  CHECK(s.m() == MotorDir::Stop);
  s.press(true);  // 紧急状态下黑色按钮无效
  CHECK(s.st() == State::Emergency);
  s.press(false, true);
  CHECK(s.st() == State::Printing);
  s.run(10);
  CHECK(s.m() == MotorDir::Forward);
}

static void test_unload() {
  title("结束打印 -> 退到就绪开关松开再多退一点 -> 已退料（耗材留在进料口）");
  Sim s(Inputs{true, true, false, true});
  s.press(true);
  s.run(100);
  s.press(true);
  CHECK(s.st() == State::Unloading);
  CHECK(s.m() == MotorDir::Reverse);
  CHECK(s.fsm.motor().speed == s.p.unloadSpeed);
  s.run(3000);
  CHECK(s.st() == State::Unloading);  // 就绪开关还有耗材，继续退
  s.in.ready = false;
  s.in.full = false;
  s.run(s.p.unloadExtraMs - 50);
  CHECK(s.st() == State::Unloading);
  CHECK(s.m() == MotorDir::Reverse);
  s.run(100);
  CHECK(s.st() == State::Retracted);
  CHECK(s.m() == MotorDir::Stop);
  // 已退料状态静置不会自己吸料
  s.run(10000);
  CHECK(s.st() == State::Retracted);
  CHECK(s.m() == MotorDir::Stop);
}

static void test_unload_cancel_and_timeout() {
  title("退料中按黑色按钮取消；退料超时");
  Sim s(Inputs{true, true, false, true});
  s.press(true);
  s.press(true);
  CHECK(s.st() == State::Unloading);
  s.press(true);
  CHECK(s.st() == State::Ready);
  CHECK(s.m() == MotorDir::Stop);
  s.press(true);
  s.press(true);
  CHECK(s.st() == State::Unloading);
  s.run(s.p.unloadTimeoutMs + 50);
  CHECK(s.st() == State::Emergency);
  CHECK(std::strcmp(s.fsm.note(), "退料超时") == 0);
}

static void test_out_of_material_while_printing() {
  title("打印中耗材用尽：不再补料，结束打印直接回到无耗材");
  Sim s(Inputs{true, true, false, true});
  s.press(true);
  s.in.in = false;
  s.in.ready = false;
  s.in.full = false;
  s.in.lack = true;
  s.run(100);
  CHECK(s.st() == State::Printing);
  CHECK(s.m() == MotorDir::Stop);
  CHECK(std::strcmp(s.fsm.note(), "耗材已用尽") == 0);
  s.press(true);
  CHECK(s.st() == State::NoMaterial);
}

static void test_refill_while_printing() {
  title("打印中耗材用尽后换上新耗材：延时后按首次送料超时重新送到满");
  Sim s(Inputs{true, true, false, true});
  s.press(true);
  s.in = Inputs{false, false, true, false};
  s.run(100);
  CHECK(s.m() == MotorDir::Stop);
  s.in.in = true;  // 插入新耗材
  s.run(300);
  CHECK(s.m() == MotorDir::Stop);  // 插入延时内不动
  s.run(300);
  CHECK(s.m() == MotorDir::Forward);
  CHECK(std::strcmp(s.fsm.note(), "") == 0);
  s.run(s.p.feedTimeoutMs + 10000);  // 超过普通补料超时也不报警
  CHECK(s.st() == State::Printing);
  CHECK(s.m() == MotorDir::Forward);
  s.in.full = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Stop);
}

static void test_retracted_reload_and_print() {
  title("已退料 -> 按黑色按钮重新吸料 -> 到就绪位置后直接开始打印送料");
  Sim s(Inputs{true, false, false, false});
  CHECK(s.st() == State::Retracted);
  s.press(true);
  CHECK(s.st() == State::Loading);
  CHECK(s.m() == MotorDir::Forward);
  CHECK(s.fsm.motor().speed == s.p.loadSpeed);
  s.run(2000);
  s.in.ready = true;
  s.run(10);
  CHECK(s.st() == State::Printing);
  s.run(10);
  CHECK(s.m() == MotorDir::Forward);
  CHECK(s.fsm.motor().speed == s.p.feedSpeed);
  s.in.full = true;
  s.run(10);
  CHECK(s.m() == MotorDir::Stop);
  // 已退料时把耗材抽出 -> 无耗材，再插入会自动吸入
  Sim r(Inputs{true, false, false, false});
  r.in.in = false;
  r.run(50);
  CHECK(r.st() == State::NoMaterial);
  r.in.in = true;
  r.run(600);
  CHECK(r.st() == State::Loading);
  r.in.ready = true;
  r.run(10);
  CHECK(r.st() == State::Ready);  // 插入触发的吸料，到位后停在就绪，不自动开始打印
}

static void test_black_during_loading() {
  title("吸料过程中按黑色按钮：吸料完成后直接开始打印");
  Sim s;
  s.in.in = true;
  s.run(600);
  CHECK(s.st() == State::Loading);
  s.press(true);
  CHECK(s.st() == State::Loading);
  CHECK(s.m() == MotorDir::Forward);
  s.in.ready = true;
  s.run(10);
  CHECK(s.st() == State::Printing);
}

static void test_unload_ready_switch_broken() {
  title("就绪开关失效（退料时从没触发）：一直退到离开进料口");
  Sim s(Inputs{true, false, false, false});
  s.press(true);  // 已退料 -> 吸料
  s.in.full = true;  // 满料霍尔兜底判就绪 -> 开始打印
  s.run(10);
  CHECK(s.st() == State::Printing);
  s.press(true);
  CHECK(s.st() == State::Unloading);
  s.run(5000);
  CHECK(s.st() == State::Unloading);
  s.in.in = false;
  s.run(s.p.unloadExtraMs + 50);
  CHECK(s.st() == State::NoMaterial);
  CHECK(s.m() == MotorDir::Stop);
}

static void test_ready_by_full_fallback_stays() {
  title("满料霍尔兜底判就绪后停在就绪，不会跳到已退料");
  Sim s;
  s.in.in = true;
  s.run(600);
  s.in.full = true;
  s.run(10);
  CHECK(s.st() == State::Ready);
  s.run(2000);
  CHECK(s.st() == State::Ready);
}

static void test_emergency_anywhere() {
  title("任何状态按红色按钮都立即停止");
  Sim s;
  s.in.in = true;
  s.run(600);
  CHECK(s.st() == State::Loading);
  s.press(false, true);
  CHECK(s.st() == State::Emergency);
  CHECK(s.m() == MotorDir::Stop);
  s.run(60000);
  CHECK(s.st() == State::Emergency);  // 紧急状态不会自己恢复
  CHECK(s.m() == MotorDir::Stop);
}

static void test_boot_states() {
  title("上电状态：不自动转电机");
  Sim a(Inputs{true, false, false, false});
  CHECK(a.st() == State::Retracted);
  a.run(2000);
  CHECK(a.st() == State::Retracted);
  CHECK(a.m() == MotorDir::Stop);
  Sim b(Inputs{true, true, false, false});
  CHECK(b.st() == State::Ready);
  Sim c;
  CHECK(c.st() == State::NoMaterial);
  c.press(true);
  CHECK(c.st() == State::NoMaterial);  // 无耗材时开始无效
}

static void test_both_halls_active() {
  title("两个霍尔同时有效（磁铁太大）时以 FULL 为准，不送料");
  Sim s(Inputs{true, true, true, true});
  s.press(true);
  s.run(50);
  CHECK(s.m() == MotorDir::Stop);
}

static void test_debouncer() {
  title("消抖");
  Debouncer d(3);
  d.reset(false);
  CHECK(!d.update(true));
  CHECK(!d.update(false));  // 抖动被重置
  CHECK(!d.update(true));
  CHECK(!d.update(true));
  CHECK(d.update(true));
  CHECK(d.value());
}

static void test_buttons() {
  title("按键短按 / 长按");
  ButtonTracker black(3, false, 3000);
  black.reset(false);
  uint32_t t = 0;
  auto feed = [&](ButtonTracker& b, bool p, uint32_t ms, int& shorts, int& longs) {
    for (uint32_t i = 0; i < ms; i += 10) {
      t += 10;
      auto r = b.update(p, t);
      shorts += r.shortPress;
      longs += r.longPress;
    }
  };
  int sh = 0, lo = 0;
  feed(black, true, 200, sh, lo);
  CHECK(sh == 0);  // 黑色按钮松开才触发
  feed(black, false, 100, sh, lo);
  CHECK(sh == 1 && lo == 0);
  sh = lo = 0;
  feed(black, true, 3500, sh, lo);
  CHECK(black.longHeld(t));
  feed(black, false, 100, sh, lo);
  CHECK(sh == 0 && lo == 1);  // 长按不触发开始/结束

  ButtonTracker red(3, true, 3000);
  red.reset(false);
  sh = lo = 0;
  feed(red, true, 50, sh, lo);
  CHECK(sh == 1);  // 红色按钮按下立即触发
  feed(red, false, 50, sh, lo);
  CHECK(sh == 1 && lo == 0);

  ButtonTracker held(3, true, 3000);
  held.reset(true);  // 上电时按着
  sh = lo = 0;
  feed(held, true, 5000, sh, lo);
  feed(held, false, 100, sh, lo);
  CHECK(sh == 0 && lo == 0);
}

static void test_motor_logic() {
  title("电机：起步渐加速、停止先刹车、换向经过刹车、慢衰减 PWM");
  MotorLogic m;
  MotorLogic::Config cfg;
  m.setConfig(cfg);
  uint32_t t = 0;
  m.tick(MotorDir::Forward, 60, t += 10);
  CHECK(m.dir() == MotorDir::Forward);
  CHECK(m.duty() == cfg.startDuty);
  // 正向：IN1 常高，IN2 反相 PWM（高 = 刹车占比）
  CHECK(m.output().in1.level == PinLevel::High);
  CHECK(m.output().in2.level == PinLevel::Pwm);
  CHECK(m.output().in2.duty == 100 - cfg.startDuty);
  for (int i = 0; i < 50; i++) m.tick(MotorDir::Forward, 60, t += 10);
  CHECK(m.duty() == 60);
  CHECK(m.output().in2.duty == 40);
  // 换向：先刹车
  m.tick(MotorDir::Reverse, 60, t += 10);
  CHECK(m.braking());
  CHECK(m.output().in1.level == PinLevel::High && m.output().in2.level == PinLevel::High);
  for (int i = 0; i < 20; i++) m.tick(MotorDir::Reverse, 60, t += 10);
  CHECK(m.dir() == MotorDir::Reverse);
  CHECK(m.output().in2.level == PinLevel::High);
  CHECK(m.output().in1.level == PinLevel::Pwm);
  // 停止：刹车后滑行
  m.tick(MotorDir::Stop, 0, t += 10);
  CHECK(m.braking());
  for (int i = 0; i < 20; i++) m.tick(MotorDir::Stop, 0, t += 10);
  CHECK(!m.braking());
  CHECK(m.output().in1.level == PinLevel::Low && m.output().in2.level == PinLevel::Low);
  // 100% 时 PWM 脚为低电平常量
  for (int i = 0; i < 60; i++) m.tick(MotorDir::Forward, 100, t += 10);
  CHECK(m.output().in1.level == PinLevel::High && m.output().in2.level == PinLevel::Low);
  // 反转设置
  MotorLogic r;
  r.setInvert(true);
  r.tick(MotorDir::Forward, 50, 10);
  CHECK(r.output().in2.level == PinLevel::High && r.output().in1.level == PinLevel::Pwm);
}

int main(int argc, char** argv) {
  g_verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
  test_auto_load();
  test_ready_switch_fallback();
  test_load_timeout();
  test_user_pulls_filament_out();
  test_no_reload_right_after_unload_pullout();
  test_printing_buffer();
  test_fast_magnet_move();
  test_feed_timeout_and_resume();
  test_unload();
  test_unload_cancel_and_timeout();
  test_out_of_material_while_printing();
  test_refill_while_printing();
  test_retracted_reload_and_print();
  test_black_during_loading();
  test_unload_ready_switch_broken();
  test_ready_by_full_fallback_stays();
  test_emergency_anywhere();
  test_boot_states();
  test_both_halls_active();
  test_debouncer();
  test_buttons();
  test_motor_logic();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail ? 1 : 0;
}
