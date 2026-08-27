// ui/coin_widget.cpp — 顶栏金币显示 + 飘字
//   * 顶栏左侧："$N"（24×12 像素，金色）
//   * 飘字：lv_anim 透明度 + Y 偏移，0.8s 上升 30px + 淡出
#include "coin_widget.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "game/coins.h"
#include "lvgl.h"
#include <cstdio>

namespace boxpet::ui {

namespace {
lv_obj_t* g_coin_label = nullptr;

// 飘字动画结束回调：自动删除 label
static void float_anim_end_cb(lv_anim_t* a) {
    lv_obj_t* obj = (lv_obj_t*)a->var;
    if (obj) lv_obj_del(obj);
}

void refresh_label() {
    if (!g_coin_label) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "$%ld", (long)boxpet::game::coins_get());
    lv_label_set_text(g_coin_label, buf);
}
}  // namespace

lv_obj_t* coin_widget_create(lv_obj_t* parent, int x, int y) {
    g_coin_label = lv_label_create(parent);
    lv_obj_set_pos(g_coin_label, x, y);
    lv_obj_set_style_text_color(g_coin_label, lv_color_hex(0xFFD700), 0);  // 金色
    lv_obj_set_style_text_font(g_coin_label, ui_font_16, 0);
    refresh_label();
    return g_coin_label;
}

void coin_widget_refresh() { refresh_label(); }

void coin_widget_float_text(int amount) {
    if (!g_coin_label) return;
    // 创建 + 动画全程必须持 LVGL 锁：lv_anim_start 会把动画插入 LVGL 全局
    // 动画链表，若在解锁后调用，与 LVGL 刷新任务并发操作链表 → 内部链表
    // 损坏 → 死机复位（esp_lvgl_port 为递归互斥锁，调用方已持锁时安全）。
    if (!lvgl_port_lock(200)) return;
    // 创建飘字 label（在根屏幕，居中位置）
    lv_obj_t* root = lv_scr_act();
    lv_obj_t* fl = lv_label_create(root);
    char buf[16];
    snprintf(buf, sizeof(buf), "%s%d", amount > 0 ? "+" : "", amount);
    lv_label_set_text(fl, buf);
    lv_color_t color = (amount > 0) ? lv_color_hex(0x00C000)   // 绿
                                    : lv_color_hex(0xC00000);  // 红
    lv_obj_set_style_text_color(fl, color, 0);
    lv_obj_set_style_text_font(fl, ui_font_16, 0);
    // 初始位置：金币图标附近
    lv_obj_set_pos(fl, 6, 26);
    // 飘字动画：0.8s 上移 30px + 淡出（持锁启动，杜绝链表竞态）
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, fl);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_y((lv_obj_t*)obj, 26 - v);
        lv_obj_set_style_opa((lv_obj_t*)obj, LV_OPA_COVER - v * 4, 0);
    });
    lv_anim_set_values(&a, 0, 30);
    lv_anim_set_time(&a, 800);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_ready_cb(&a, float_anim_end_cb);
    lv_anim_start(&a);
    lvgl_port_unlock();
}

}  // namespace boxpet::ui