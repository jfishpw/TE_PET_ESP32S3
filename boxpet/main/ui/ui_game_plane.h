// ui/ui_game_plane.h — 飞机打害虫小游戏（竖版射击 + lv_canvas）
#pragma once
#include "lvgl.h"

namespace boxpet::ui {

// 创建飞机游戏页 root 对象
lv_obj_t* ui_plane_create();

// 查询/清除退出标志（主循环用）
bool ui_plane_wants_to_leave();
void ui_plane_clear_leave_flag();

// 退出时最后一次战绩（主循环结算金币用）
int  ui_plane_last_hits();
bool ui_plane_last_time_up();

// 关闭游戏页（清理 esp_timer + 删除 lv_obj）
void ui_plane_close();

}  // namespace boxpet::ui