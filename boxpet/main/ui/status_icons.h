// status_icons.h — 主界面低状态图标（宠物区四角，需求：低于60明显体现）
//   饱食<60 → 鸡腿图标（饿）      心情<60 → 泪滴图标（伤心）
//   卫生<60 → 臭气+苍蝇图标（脏）  精力<60 → "Z" 图标（累）
// 图标画布由 status_icons_create 挂在主界面宠物区四角，与表情帧
// （anim 的 bad/scold/sick 联动）叠加：表情给"难受"的肢体语言，
// 图标给明确的状态语义，四种低状态可同时显示。
#pragma once
#include <cstdint>
#include "lvgl.h"
#include "game/pet.h"

namespace boxpet::ui {

// 创建 4 个图标画布（挂 parent，宠物区四角；随主界面常驻，无需显式销毁）
void status_icons_create(lv_obj_t* parent);

// 每 tick 调用（需持 LVGL 锁）：刷新可见性/浮动动画/昼夜背景重绘
void status_icons_update(const ::boxpet::game::PetState& st, int64_t now_ms, bool light_on);

}  // namespace boxpet::ui
