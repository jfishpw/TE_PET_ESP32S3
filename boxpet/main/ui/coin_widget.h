// ui/coin_widget.h — 顶栏金币显示 + 飘字动画
#pragma once
#include "lvgl.h"

namespace boxpet::ui {

// 创建顶栏金币组件：返回根容器（lv_obj_t*），挂在指定父对象 + 坐标
// 显示效果：金币图标（"¢"或"$"字符）+ 数字
lv_obj_t* coin_widget_create(lv_obj_t* parent, int x, int y);

// 更新金币数字（外部 NVS 变动后主动调用）
void coin_widget_refresh();

// 在顶栏飘出 "+N" 飘字动画（从金币图标位置向上飘 30px 再淡出）
// amount > 0 = 获得；< 0 = 消费
void coin_widget_float_text(int amount);

}  // namespace boxpet::ui