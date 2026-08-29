// ui/ui_netcfg.h — 配网模式提示页（需求4）
// 进入即开启 SoftAP + Web 配置服务；屏幕显示热点信息；长按中键退出。
#pragma once

#include "lvgl.h"

namespace boxpet::ui {

lv_obj_t* ui_netcfg_create();
void ui_netcfg_close();
bool ui_netcfg_wants_leave();
void ui_netcfg_clear_leave_flag();

}  // namespace boxpet::ui
