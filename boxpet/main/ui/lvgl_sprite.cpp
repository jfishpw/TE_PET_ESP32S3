// lvgl_sprite.cpp — 4bpp 彩色 sprite 渲染到 LVGL canvas
// 实现：直接写 canvas 缓冲区（RGB565），48x48 → 2x 缩放 = 96x96，
//       每精灵像素画 2x2 块，颜色查全局 16 色调色板。
#include "lvgl_sprite.h"
#include <cstring>

namespace boxpet::ui {

static constexpr int kScale  = 2;
static constexpr int kSrcW   = 48;
static constexpr int kSrcH   = 48;
static constexpr int kDstW   = kSrcW * kScale;   // 96
static constexpr int kDstH   = kSrcH * kScale;

// 预转换调色板（首次渲染时填充）
static lv_color_t s_pal[16];
static bool s_pal_ready = false;

static void ensure_palette() {
    if (s_pal_ready) return;
    for (int i = 0; i < 16; ++i) {
        s_pal[i] = lv_color_hex(sprites::kPalette[i]);
    }
    s_pal_ready = true;
}

// canvas 销毁时释放缓冲（避免游戏页反复进出泄漏）
static void canvas_delete_cb(lv_event_t* e) {
    void* buf = lv_event_get_user_data(e);
    if (buf) lv_free(buf);
}

lv_obj_t* create_pet_canvas_at(lv_obj_t* parent, int x, int y) {
    // 每个 canvas 独立缓冲（主界面/游戏页并存时互不干扰）
    lv_color_t* canvas_buf = (lv_color_t*)lv_malloc(kDstW * kDstH * sizeof(lv_color_t));
    if (!canvas_buf) return nullptr;
    lv_obj_t* canvas = lv_canvas_create(parent);
    lv_canvas_set_buffer(canvas, canvas_buf, kDstW, kDstH, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(canvas, kDstW, kDstH);
    lv_obj_set_pos(canvas, x, y);
    lv_obj_set_style_bg_opa(canvas, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(canvas, canvas_delete_cb, LV_EVENT_DELETE, canvas_buf);
    return canvas;
}

lv_obj_t* create_pet_canvas(lv_obj_t* parent) {
    // 主界面：宠物区 y=70~194（高124），96×96 居中 → x=72, y=84
    return create_pet_canvas_at(parent, (240 - kDstW) / 2, 70 + (124 - kDstH) / 2);
}

void render_pet_sprite(lv_obj_t* canvas, const sprites::Sprite* s, lv_color_t bg) {
    if (!canvas || !s) return;
    ensure_palette();
    // 全部走 LVGL 官方 canvas API：与颜色格式/字节序（RGB565/swap 等）完全解耦，
    // 杜绝直写缓冲造成的竖条马赛克。
    lv_canvas_fill_bg(canvas, bg, LV_OPA_COVER);
    // 4bpp：高半字节 = 偶数列像素，低半字节 = 奇数列像素
    for (int y = 0; y < kSrcH; ++y) {
        for (int x = 0; x < kSrcW; ++x) {
            uint8_t byte = s->bitmap[(y * kSrcW + x) / 2];
            uint8_t idx = (x & 1) ? (byte & 0x0F) : (byte >> 4);
            if (idx == 0) continue;   // 透明 → 背景已填
            lv_color_t c = s_pal[idx];
            // 2x2 块（本 LVGL 版本 API：lv_canvas_set_px(obj,x,y,color,opa)）
            int bx = x * kScale, by = y * kScale;
            for (int dy = 0; dy < kScale; ++dy)
                for (int dx = 0; dx < kScale; ++dx)
                    lv_canvas_set_px(canvas, bx + dx, by + dy, c, LV_OPA_COVER);
        }
    }
    lv_obj_invalidate(canvas);
}

}  // namespace boxpet::ui
