// power_mgr.h — 电源管理：背光超时 / 睡眠联动 / CPU 调频
#pragma once
#include "esp_err.h"
#include "game/pet.h"

namespace boxpet::bsp {

esp_err_t power_mgr_init(::boxpet::game::PetCore* pet);

// 任意按键触发（由 ui_main/buttons 在收到按键时调用）
void power_mgr_on_user_input();

// 获取当前是否处于"自动熄屏"状态
bool power_mgr_is_backlight_off();

// 熄屏持续时长（ms）；屏幕点亮时返回 0
int64_t power_mgr_backlight_off_ms();

// 提醒亮屏：仅当熄屏 ≥30s 时点亮（避免刚熄屏就被琐碎提醒打扰），
// 亮屏后视为一次用户输入 → 复用现有熄屏超时自动关闭
void power_mgr_wake_for_alert();

}  // namespace boxpet::bsp