// SEMI-AMS 主控（ESP32-C3）—— 常量配置
#pragma once

#define FW_NAME    "SEMI-AMS-CTRL"
#define FW_VERSION "0.2.7"

// ---------------- 网络 ----------------
#define CTRL_AP_NAME   "SEMI-AMS-CTRL"  // 配网热点（没连上路由器时开）
#define CTRL_HOSTNAME  "semi-ams-ctrl"  // http://semi-ams-ctrl.local
#define WIFI_CONNECT_TIMEOUT_S 15       // 上电等已保存 WiFi 的时间，超时开配网热点
#define WIFI_RETRY_INTERVAL_MS 10000UL  // 运行中掉线后，每隔这么久催一次重连
#define WIFI_LOST_RESTART_MS   180000UL // 连续掉线这么久还没回来就重启（开机连不上会开配网热点）

// ---------------- 设备表 ----------------
// 界面上不再固定行数：有几台就显示几行（devices.cpp 里按需扩容）。
// 这个值只是存储保护上限（NVS 里的键数量、内存），不是界面行数；
// 单个 Device 约 160 字节，32 台也就 5 KB，正常永远碰不到
#define DEVICES_MAX        32
#define DEVICE_OFFLINE_MS  90000        // 超过这么久没收到消息就当作离线（设备 30 秒一次心跳）

// ---------------- 内置 MQTT 服务端 ----------------
#define MQTT_PORT_DEFAULT      1883

// 同时接几个客户端：代码里**不设人数上限**，来一个分配一个，内存不够才拒。
// 真正的天花板是 socket 数——ESP32-C3 的 sdkconfig 里 CONFIG_LWIP_MAX_SOCKETS=16，
// 这个常量编译进了 IDF 静态库，改头文件或加 build_flags 都不生效（除非自己重编 IDF）。
// 16 个里扣掉 2 个监听口（80 网页 / 1883 MQTT）、再给网页留 2 个并发请求，剩 12 个给 MQTT。
// 想再放开可以调小 MQTT_SOCKET_RESERVE_WEB（代价是客户端占满后网页可能打不开）。
#define MQTT_SOCKET_RESERVE_WEB 2
#define MQTT_LISTEN_BACKLOG     8       // listen backlog，只影响同时握手的排队长度
#define MQTT_MIN_FREE_HEAP      20000   // 堆低于这个值就不再收新连接，避免把网页/系统挤死
#define MQTT_TOPIC_MAX         64       // 主题最长字节数（含结尾 0）
#define MQTT_SUBS_PER_CLIENT   6        // 每个客户端最多几个订阅过滤器（进料器只用 2 个）
#define MQTT_SUB_LEN           48
#define MQTT_BODY_MAX          768      // 单条报文最大字节数（state 约 300）
#define MQTT_KEEPALIVE_LIMIT_S 7200     // 客户端声明的 keepalive 上限，超过就按这个算
#define MAX_RETAINED           24       // 保留消息条数上限（8 台 × state/availability = 16）
#define RETAINED_BYTES_MAX     10240    // 保留消息总字节上限
