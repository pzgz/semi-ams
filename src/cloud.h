// 点灯 Blinker 远程控制（控件名与"app控制界面配置.txt"一致）
#pragma once

namespace cloud {
void begin(const char* authKey);  // WiFi 连上后调用一次
void loop();
bool started();
const char* statusText();  // 未启用 / 连接中 / 已连接
void showRestarting();     // 重启前通知 App
}  // namespace cloud
