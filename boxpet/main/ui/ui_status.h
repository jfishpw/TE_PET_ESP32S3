// ui_status.h — 状态页（spec §3.5 第 6 项）
#pragma once
#include "lvgl.h"
#include "game/pet.h"

namespace boxpet::ui {

lv_obj_t* ui_status_create();
void ui_status_set_pet(::boxpet::game::PetCore* pet);
void ui_status_close();

bool ui_status_wants_leave();
void ui_status_clear_leave_flag();

}  // namespace boxpet::ui