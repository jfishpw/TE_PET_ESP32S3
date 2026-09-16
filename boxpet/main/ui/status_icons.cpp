// status_icons.cpp — 主界面低状态图标（宠物区四角）
// 4 枚 12x12 像素画 @2x = 24x24 canvas（RGB565，背景填天空/草地色与场景融合）。
// 索引色复用全局 16 色调色板（sprites.h kPalette），'.' = 透明。
#include "status_icons.h"
#include "sprites/sprites.h"
#include "game/pet_def.h"
#include "weather_bg.h"

namespace boxpet::ui {

namespace {

using game::PetState;
using game::PetStateKind;
using game::Stage;

constexpr int kArtSize = 12;                 // 素材边长（像素）
constexpr int kScale   = 2;                  // 渲染倍率 → 24x24 屏幕
constexpr int kCanvas  = kArtSize * kScale;  // 画布边长（屏幕像素）

// 槽位（宠物区四角；避开宠物 canvas x72-168 / toast y74-98 主体 / 顶底图标行）
//   天空区两枚 y=76（随昼夜换天空底色），草地两枚 y=164（随昼夜换草地底色）
struct IconSlot { int x, y; bool in_grass; };
constexpr IconSlot kSlots[4] = {
    { 36,  76, false},   // 0 饱食（饿）
    {168,  76, false},   // 1 心情（伤心）
    { 36, 164, true },   // 2 卫生（脏）
    {168, 164, true },   // 3 精力（累）
};

// ===== 像素画（12x12，'.'=透明，0-f = kPalette 索引）=====

// 鸡腿（饿）：橙肉 + 白骨
constexpr const char* kArtHungry[12] = {
    "............",
    "....9999....",
    "..99999999..",
    "..99999999..",
    "..99999999..",
    "...999999...",
    "....9999.22.",
    ".........22.",
    ".........22.",
    "........222.",
    "............",
    "............",
};

// 泪滴（伤心）：蓝水滴
constexpr const char* kArtSad[12] = {
    "............",
    "......7.....",
    ".....77.....",
    ".....777....",
    "....7777....",
    "....77777...",
    "...7777777..",
    "...7777777..",
    "..77777777..",
    "..77777777..",
    "...777777...",
    ".....777....",
};

// 臭气 + 苍蝇（脏）：绿色波纹 + 右上小蝇
constexpr const char* kArtDirty[12] = {
    "............",
    ".d..d....e.e",
    "d..d......ff",
    ".d..d....e.e",
    "d..d........",
    ".d..d.......",
    "d..d........",
    ".d..d.......",
    "d..d........",
    ".d..d.......",
    "............",
    "............",
};

// "Z"（累）：灰蓝 Z 字（区别于睡觉状态的 zzz 精灵帧）
constexpr const char* kArtTired[12] = {
    "............",
    ".fffffffff..",
    "........ff..",
    ".......ff...",
    "......ff....",
    ".....ff.....",
    "....ff......",
    "...ff.......",
    "..ff........",
    ".ffffffffff.",
    "............",
    "............",
};

// 每枚图标一组 12 行
using IconArt = const char* const*;
constexpr IconArt kIconArts[4] = {
    kArtHungry, kArtSad, kArtDirty, kArtTired,
};

struct IconCtx {
    lv_obj_t* canvas    = nullptr;
    bool      visible   = false;
    bool      has_bg    = false;        // 是否已渲染过底色
    lv_color_t last_bg  = {};           // 上次渲染的底色（昼夜/天气变化需重绘）
    int       last_y_off   = 0;
};
IconCtx s_icons[4];
bool    s_created = false;

static void hex_to_palette_idx(char ch, int* out) {
    if (ch >= '0' && ch <= '9') { *out = ch - '0'; return; }
    if (ch >= 'a' && ch <= 'f') { *out = ch - 'a' + 10; return; }
    *out = -1;
}

static void render_icon(IconCtx& ic, IconArt art, lv_color_t bg) {
    lv_canvas_fill_bg(ic.canvas, bg, LV_OPA_COVER);
    for (int y = 0; y < kArtSize; ++y) {
        const char* row = art[y];
        for (int x = 0; x < kArtSize; ++x) {
            int idx;
            hex_to_palette_idx(row[x], &idx);
            if (idx <= 0) continue;   // 0=透明占位/未知
            lv_color_t c = lv_color_hex(sprites::kPalette[idx]);
            int bx = x * kScale, by = y * kScale;
            for (int dy = 0; dy < kScale; ++dy)
                for (int dx = 0; dx < kScale; ++dx)
                    lv_canvas_set_px(ic.canvas, bx + dx, by + dy, c, LV_OPA_COVER);
        }
    }
    lv_obj_invalidate(ic.canvas);
}

static lv_color_t slot_bg(const IconSlot& slot, bool light_on) {
    // 天气感知底色（与天气背景层一致，避免图标画布露出不匹配的方块）
    if (slot.in_grass) return weather_bg_grass_color(light_on);
    return weather_bg_sky_color(light_on);
}

static void canvas_del_cb(lv_event_t* e) {
    void* buf = lv_event_get_user_data(e);
    if (buf) lv_free(buf);
}

static lv_obj_t* make_icon_canvas(lv_obj_t* parent, int x, int y) {
    // 画布缓冲走 LVGL 内部池（与宠物画布同策略，见 lvgl_sprite.cpp 注释）：
    // 内部 SRAM 最稳；池余量充足（128KB 实测占用 ~18%）
    lv_color_t* buf = (lv_color_t*)lv_malloc(
        kCanvas * kCanvas * sizeof(lv_color_t));
    if (!buf) return nullptr;
    lv_obj_t* c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, buf, kCanvas, kCanvas, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(c, kCanvas, kCanvas);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(c, canvas_del_cb, LV_EVENT_DELETE, buf);
    return c;
}

}  // namespace

void status_icons_create(lv_obj_t* parent) {
    if (s_created) return;   // 主界面 root 常驻不重建，防重复创建
    if (!parent) return;
    for (int i = 0; i < 4; ++i) {
        s_icons[i] = IconCtx{};
        s_icons[i].canvas = make_icon_canvas(parent, kSlots[i].x, kSlots[i].y);
    }
    s_created = true;
}

void status_icons_update(const PetState& st, int64_t now_ms, bool light_on) {
    if (!s_created) return;
    // 蛋期无属性外观；睡觉/死亡不叠图标（睡觉有 zzz 帧，死亡有墓碑）
    bool base = st.stage != Stage::Egg && st.stage != Stage::Dead
             && st.pstate != PetStateKind::SLEEPING
             && st.pstate != PetStateKind::DEAD;
    const bool show[4] = {
        base && st.hunger  <  game::kLowHungerIdleThreshold,    // 饿
        base && st.mood    <  game::kLowMoodIdleThreshold,      // 伤心
        base && st.hygiene <  game::kLowHygieneIdleThreshold,   // 脏
        base && st.energy  <  game::kIdleTiredEnergy,           // 累
    };
    const int bob = ((now_ms / 400) % 2) ? -2 : 0;   // 轻微上下浮动
    for (int i = 0; i < 4; ++i) {
        IconCtx& ic = s_icons[i];
        if (!ic.canvas) continue;
        if (show[i] != ic.visible) {
            ic.visible = show[i];
            if (show[i]) lv_obj_clear_flag(ic.canvas, LV_OBJ_FLAG_HIDDEN);
            else         lv_obj_add_flag(ic.canvas, LV_OBJ_FLAG_HIDDEN);
            ic.has_bg = false;   // 强制重绘一次（底色/内容）
        }
        if (!ic.visible) continue;
        // 底色随昼夜 + 天气：底色变了就重绘（否则图标画布会露出与背景不一致的方块）
        lv_color_t bg = slot_bg(kSlots[i], light_on);
        if (!ic.has_bg || !lv_color_eq(bg, ic.last_bg)) {
            render_icon(ic, kIconArts[i], bg);
            ic.last_bg = bg;
            ic.has_bg  = true;
        }
        if (bob != ic.last_y_off) {
            lv_obj_set_y(ic.canvas, kSlots[i].y + bob);
            ic.last_y_off = bob;
        }
    }
}

}  // namespace boxpet::ui
