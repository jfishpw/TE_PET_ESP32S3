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

// 唤醒吞键宽限期：GPIO 唤醒 Light Sleep 后 500ms 内的按键边沿
// 视为"唤醒键"（只亮屏不操作），由 buttons.cpp 查询
bool power_mgr_wake_grace_active();

// 事件预测器：返回"距下一个需亮屏事件的秒数"（60~4294），
// 由 ui_main 注册（计算便便/饥饿/卫生/生病/死亡/睡眠时段等事件点），
// 独立睡眠任务据此设 RTC 定时唤醒
using WakePredictor = int64_t (*)();
void power_mgr_set_wake_predictor(WakePredictor fn);

// Light Sleep 入口（由内部睡眠任务调用）：CPU 暂停，RTC+SRAM 保持。
// 唤醒源：① RTC timer 到点（事件不漏）② 任意按键（gpio_wakeup）。
// 按键唤醒后自动亮屏 + 进入 500ms 吞键宽限期；esp_timer 醒来自动补跳。
void power_mgr_enter_light_sleep(int64_t wake_after_sec);

}  // namespace boxpet::bsp