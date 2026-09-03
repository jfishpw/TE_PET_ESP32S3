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

// ===== 深休眠（ESP32-S3 重启式；睡眠改写 v3：真实模式宠物睡觉全天深睡）=====
enum : uint8_t {
    kDsReasonNone    = 0,  // 无需续睡（正常开机）
    kDsReasonNight   = 1,  // 夜间宠物睡眠：睡到起床点（<50μA 级）
    kDsReasonBattery = 2,  // 电量≤10% 应急：每 30min 自醒自检 + 左右键唤醒
    kDsReasonNap     = 3,  // 白天睡觉（精疲力竭/手动哄睡）：睡到精力满自动醒
};
// 睡眠任务调：满足深休眠条件（真实模式宠物 SLEEPING：夜间/白天小睡；或低电量）
// 则存档入深休眠（不返回）；不满足返回 false，走 Light Sleep。
bool power_mgr_try_deep_sleep();

// 开机早期（app_main 在 board/UI 初始化前）调：返回是否刚从深休眠唤醒，
// 带出睡眠真实秒数 / 原因 / 是否已插充电。调用后内部标记即消费（幂等）。
bool power_mgr_deep_sleep_resume(int64_t* elapsed_sec, uint8_t* reason, bool* charging);

// 补跳后"是否该继续睡"判定：宠物仍 SLEEPING 且未充电 → 返回续睡原因
// （窗口内=Night / 窗口外=Nap）；已醒或在充电 → kDsReasonNone（正常开机）。
uint8_t power_mgr_deep_sleep_continue_reason();

// 唤醒后决策为"续睡"（仍处深夜窗口 / 仍未充电）→ 再入深休眠（不返回）。
// no_btn_wake=true 时续睡不再启用左键 EXT1 唤醒：用于 EXT1 毛刺假唤醒后的
// 续睡（strapping 脚 GPIO3 入睡瞬间电平毛刺会反复误触 EXT1，形成
// "睡着→假醒→续睡"循环；真人按键在首睡时已可通过毛刺防护正常开机）。
void power_mgr_reenter_deep_sleep(uint8_t reason, bool no_btn_wake = false);

// 当前真实小时是否处于宠物睡眠窗口（跨午夜支持）。深休眠续睡判定用。
bool power_mgr_in_pet_sleep_window();

}  // namespace boxpet::bsp