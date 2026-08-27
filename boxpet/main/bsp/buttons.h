// bsp/buttons.h — 3 物理按键（中断 + 去抖 + 长按判定）
#pragma once

#include <stdint.h>
#include <functional>
#include "esp_err.h"

namespace boxpet::bsp {

enum class KeyId : uint8_t {
    Left  = 0,
    Mid   = 1,
    Right = 2,
};

enum class KeyEvent : uint8_t {
    ShortPress = 0,   // 短按（按下后 < 1500ms 释放）
    LongPress  = 1,   // 长按（按下 >= 1500ms 后自动触发一次）
    Release    = 2,   // 长按持续按住时，每 200ms 重复一次（保留，本期不用）
};

using KeyCallback = std::function<void(KeyId id, KeyEvent evt)>;

// 初始化 3 个按键 GPIO 与定时器。
// 必须在 board_init 之后调用（依赖 LVGL/lvgl_port 启动后才能投递事件）。
esp_err_t buttons_init();

// 注册按键事件回调（支持多个，最后注册的覆盖）
void buttons_set_callback(KeyCallback cb);

// 查询按键是否当前处于"已确认按下"状态（持续按住期间持续返回 true）。
// 用于飞机游戏等需要"按住期间持续移动"的场景。
// 注意：返回值基于 100Hz 轮询的最新消抖状态，最大滞后 ~10ms。
bool buttons_is_held(KeyId id);

}  // namespace boxpet::bsp