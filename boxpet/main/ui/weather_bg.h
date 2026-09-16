// ui/weather_bg.h — 天气背景层（画布绘制天气形态 + 温度文字）
//   用一块 240x124 画布（覆盖天空 y70..162 + 草地 y162..194）绘制天气：
//   每帧只一次 invalidate（对比"几十个小对象移动"的方案，lv_inv_area 调用
//   降到最低——该函数曾在损坏的 LVGL 池数据上空转导致看门狗复位）。
#pragma once
#include <cstdint>
#include "lvgl.h"

namespace boxpet::ui {

// 创建天气画布 + 天气/温度标签（须在天空/草地对象之后、宠物画布之前创建）
void weather_bg_create(lv_obj_t* parent);

// 每 tick 调用（需持 LVGL 锁）：按需重绘（天气动画 5Hz；昼夜/天气变化立即重绘）
void weather_bg_update(int64_t now_ms, bool light_on);

// 天气感知的天空/草地底色：宠物画布、状态图标画布需用它作底，
// 否则天气变灰/变暗时这些不透明画布会露出"明显偏亮的方块"
lv_color_t weather_bg_sky_color(bool light_on);
lv_color_t weather_bg_grass_color(bool light_on);

}  // namespace boxpet::ui
