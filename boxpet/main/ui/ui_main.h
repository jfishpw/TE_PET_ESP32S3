// ui/ui_main.h — 主界面（占位 + 8 图标 + 顶栏 + 按键联动）
#pragma once

#include "lvgl.h"
#include "bsp/buttons.h"
#include "game/pet.h"

namespace boxpet::ui {

// 创建主界面对象并返回根 screen（默认即激活）。
// 仅在 LVGL 线程内调用。
lv_obj_t* ui_main_create();

// 注册按键事件（短按：左右移动焦点 / 中键确认；长按：中键进入设置）
void ui_main_attach_key(bsp::KeyCallback cb);

// 注入宠物状态机（用于事件驱动 UI 刷新）
void ui_main_attach_pet(::boxpet::game::PetCore* pet);

// 启动主界面内部 1Hz 定时器（用于刷新时钟/电量/注意图标闪烁）
void ui_main_start_tick(uint8_t hours, uint8_t minutes);

// 查询并清除："用户希望进入游戏场景"
bool ui_main_consume_want_game();

// 游戏场景模式（在 consume_want_game 返回 true 后读取）
// is_edu=false → kind 为 game::PlayKind；true → game::EduKind
bool     ui_main_pending_is_edu();
uint8_t  ui_main_pending_kind();

// 查询并清除："用户希望进入状态页"
bool ui_main_consume_want_status();

// 查询并清除："用户希望进入设置"
bool ui_main_consume_want_settings();

// 查询并清除："用户希望进入商店"
bool ui_main_consume_want_shop();

// 查询并清除："用户希望进入飞机打害虫游戏"
bool ui_main_consume_want_plane();

// 查询并清除："用户希望重置（死亡后长按中键孵化新蛋）"
bool ui_main_consume_want_resurrect();

// 显示一条 toast 提示（duration_ms 后自动消失）
void ui_main_show_toast(const char* text, int duration_ms);

// 触发进化光效（白色光晕闪烁 + 形态交替，用于"穿越/重生"等游戏事件）
void ui_main_trigger_evolve_fx();

// UI tick 心跳（挂死看门狗监视用，power_mgr 睡眠任务轮询）：
// 返回最近一次成功渲染 tick 的 esp_timer 毫秒；僵死时停摆。
int64_t ui_main_last_tick_ms();

}  // namespace boxpet::ui