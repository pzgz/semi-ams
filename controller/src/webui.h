// 网页和 REST 接口
#pragma once
#include <Arduino.h>

namespace ctrl {
namespace webui {

// portal = true：配网模式（热点 + 只提供配网页面）
void begin(bool portal);
void loop();

}  // namespace webui
}  // namespace ctrl
