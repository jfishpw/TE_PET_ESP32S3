// ui_game.h — 小游戏/课程场景（玩耍 + 教育，需求 §2.3/§2.5）
// 由 main 在进入前调用 ui_game_configure() 指定模式
#pragma once
#include "lvgl.h"
#include "game/pet.h"

namespace boxpet::ui {

// is_edu=false: kind=PlayKind；true: kind=EduKind
void ui_game_configure(bool is_edu, uint8_t kind);

lv_obj_t* ui_game_create();
void ui_game_start();
void ui_game_set_pet(::boxpet::game::PetCore* pet);
void ui_game_close();

// 中键短按确认 / 长按退出游戏 → 由 main 轮询
bool ui_game_wants_to_leave();
void ui_game_clear_leave_flag();

}  // namespace boxpet::ui
