// lvgl_sprite.h — 4bpp 彩色 sprite 渲染到 lv_canvas（48x48 → 2x = 96x96）
#pragma once
#include "lvgl.h"
#include "sprites/sprites.h"

namespace boxpet::ui {

// 创建宠物精灵画布（默认主界面位置：宠物区中央）
lv_obj_t* create_pet_canvas(lv_obj_t* parent);

// 创建宠物精灵画布（自定义位置，游戏页等用）
lv_obj_t* create_pet_canvas_at(lv_obj_t* parent, int x, int y);

// 把 4bpp sprite 按全局调色板渲染到画布（背景色 bg，2x 缩放）
void render_pet_sprite(lv_obj_t* canvas, const sprites::Sprite* s, lv_color_t bg);

}  // namespace boxpet::ui
