// ui/ui_shop.h — 迷你商店页面（列表式 + 三按键）
#pragma once
#include "lvgl.h"
#include "game/pet.h"

namespace boxpet::ui {

// 创建商店页面 root 对象
lv_obj_t* ui_shop_create();

// 绑定宠物（用于查询库存 / 清冷却）
void ui_shop_set_pet(::boxpet::game::PetCore* pet);

// 查询/清除退出标志（主循环用）
bool ui_shop_wants_to_leave();
void ui_shop_clear_leave_flag();

// 关闭（清理 esp_timer + 删除 lv_obj）
void ui_shop_close();

// 主循环轮询：执行挂起的用药效果（购买按键时只记标记，medicate 的事件
// 级联不在按键任务+LVGL 锁内跑——实测该上下文里触发事件链会整系统僵死）
void ui_shop_poll();

}  // namespace boxpet::ui