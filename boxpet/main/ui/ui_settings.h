// ui_settings.h — 设置菜单（长按中键进入）
#pragma once
#include "lvgl.h"
#include "game/pet.h"

namespace boxpet::ui {

lv_obj_t* ui_settings_create();
void ui_settings_set_pet(::boxpet::game::PetCore* pet);
void ui_settings_close();

bool ui_settings_wants_leave();
void ui_settings_clear_leave_flag();

// 设置菜单是否请求"重置存档"
bool ui_settings_wants_reset();
void ui_settings_clear_reset_flag();

// 设置菜单是否请求"进入配网模式"（需求4）
bool ui_settings_wants_netcfg();
void ui_settings_clear_netcfg_flag();

}  // namespace boxpet::ui