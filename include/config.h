// SEMI-AMS 复刻固件 —— 硬件与常量配置
// 引脚按 Netlist_Schematic2（ESP-12S）核对
#pragma once

#define FW_NAME    "SEMI-AMS"
#define FW_VERSION "1.2.3"

// ---------------- 引脚 ----------------
#define PIN_HALL_EMPTY    12  // U11 EMPTY_CHECK（DRV5032FA 推挽），低电平 = 缓冲器空
#define PIN_HALL_FULL     13  // U12 FULL_CHECK，低电平 = 缓冲器满
#define PIN_SW_IN          4  // U13 IN_CHECK 微动开关（接地），低电平 = 进料口有耗材
#define PIN_SW_READY       5  // U14 READY_CHECK 微动开关（接地），低电平 = 耗材就绪
                              // 注意：GPIO5 同时直接驱动缺料红灯的 MOS 管，绝对不能设为输出
#define PIN_BTN_START      0  // U18 黑色按钮 START-STOP，按下为低（R3 10k 上拉）
#define PIN_BTN_EMERGENCY 15  // U19 红色按钮 EMERGENCY，按下为高（R14 4.7k 接 3V3）
#define PIN_MOTOR_IN1     14  // DRV8870 IN1
#define PIN_MOTOR_IN2     16  // DRV8870 IN2
#define PIN_LED            2  // ESP-12S 板载蓝灯，低电平点亮

// ---------------- 网络 ----------------
#define AP_NAME        "SEMI-AMS"   // 配网热点名称
#define AP_PASSWORD    ""           // 配网热点密码（至少 8 位；留空 = 开放热点，和原厂一致）
#define HOSTNAME       "semi-ams"   // http://semi-ams.local
#define OTA_PASSWORD   "semi-ams"   // ArduinoOTA 和网页升级（用户名 admin）的密码，建议修改
#define WIFI_CONNECT_TIMEOUT_S 15   // 上电连接已保存 WiFi 的等待时间
#define OFFLINE_PORTAL_AFTER_S 180  // 已配过网但连不上：先后台重连这么久（等路由器启动），仍连不上再开配网热点
#define PORTAL_TIMEOUT_S      300   // 上面这种情况下配网热点开放多久，之后继续后台重连（从没配过网时热点一直开着）

// ---------------- 时间 ----------------
#define TICK_MS               10    // 传感器采样 + 状态机周期
#define HALL_DEBOUNCE          2    // 霍尔：连续 2 次（20ms）一致
#define SWITCH_DEBOUNCE        5    // 微动开关：50ms
#define BUTTON_PRESS_SAMPLES   2    // 按键按下：连续 2 次（20ms）确认
#define BUTTON_RELEASE_SAMPLES 5    // 按键松开：连续 5 次（50ms）确认，按住时的短暂接触不良不算松开
#define EMG_PRESS_SAMPLES      6    // 红色按钮按下：连续 6 次（60ms）才确认。GPIO15 上有干扰脉冲，20ms 会被误触发
                                    // 进入紧急停止；人按一下一般在 100ms 以上
#define TICK_LATE_MS          40    // 10ms 周期被推迟超过这个值就记一次（诊断用）
#define LONG_PRESS_MS       3000    // 长按任意键松开后重启
#define AUTO_RESTART_DAYS      7    // 运行满 7 天，空闲时自动重启
#define CLOUD_SETTLE_MAX_MS 90000   // 点灯这么久还没完成初始化，打印机就不再等它
#define UI_PUSH_INTERVAL_MS 1500    // App 刷新最小间隔（点灯限制设备 1 秒最多发 1 条）
#define JOG_MAX_MS         10000    // 手动点动最长时间

// ---------------- 电机 ----------------
#define MOTOR_PWM_HZ        5000    // PWM 频率
#define MOTOR_START_DUTY      25    // 起步占空比 %
#define MOTOR_RAMP_STEP        3    // 每 10ms 增加的占空比 %
#define MOTOR_BRAKE_MS       150    // 停止 / 换向刹车时间
