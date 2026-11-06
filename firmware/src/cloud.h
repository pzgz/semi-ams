// 点灯 Blinker 远程控制（控件名与"app控制界面配置.txt"一致）
#pragma once

namespace cloud {
void begin(const char* authKey);  // WiFi 连上后调用一次
void loop();
bool started();
// 点灯第一次连上云端时要做一轮初始化（同步时间、HTTP 请求、两次 4KB 的 EEPROM 缓冲），内存需求最大；
// 拓竹打印机的 TLS 连接要占 12KB 左右，两者同时进行内存不够，会崩溃重启。
// 返回 true 表示打印机可以连接了：不用点灯，或点灯已初始化完，或等了 CLOUD_SETTLE_MAX_MS 还没好
bool settled();
const char* statusText();  // 未启用 / 连接中 / 已连接（在 Flash 里：用 %S 或 FPSTR 读）
void showRestarting();     // 重启前通知 App
}  // namespace cloud
