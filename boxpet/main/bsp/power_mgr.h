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

// ui_main 计算下一个"事件"还有多少秒（便便/饥饿/卫生/生病/死亡/特殊事件），
// 喂给 power_mgr，以便在 Light Sleep 中设 RTC timer wakeup
void power_mgr_set_next_event_sec(int64_t sec);
int64_t power_mgr_get_next_event_sec();

// Light Sleep 入口：CPU 暂停，RTC+SRAM 保持（约 0.8mA），可被：
//   1. RTC timer 自动唤醒（wake_after_sec 秒后）
//   2. GPIO ext0 唤醒（任意按键）
// 入参 wake_after_sec 必须 > 0 且 ≤ 4294（esp_sleep 限制）。
// 醒来后屏幕保持黑屏（未点亮）→ 调用 power_mgr_on_user_input() 才会点亮。
// 函数不返回，直到被唤醒；唤醒后 esp_timer 自动补跳所有堆积 tick。
void power_mgr_enter_light_sleep(int64_t wake_after_sec);

}  // namespace boxpet::bsp