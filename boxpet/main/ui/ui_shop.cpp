// ui/ui_shop.cpp — 商店页面实现
//   商品列表（4 项）：
//     零食      5 金币   清 2h  冷却
//     高级料理  30 金币  清 6h  冷却
//     最爱食物  80 金币  清 12h 冷却
//     特效药    50 金币  直接 +1 库存（不发食物/药冷却，叠加使用次数）
//   交互：左/右切换商品，中键确认购买（光标焦点）
#include "ui_shop.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/audio.h"
#include "game/coins.h"
#include "game/pet.h"
#include "game/pet_def.h"
#include "esp_log.h"
#include <cstdio>

static const char* TAG = "ui_shop";

namespace boxpet::ui {

namespace {

using game::PetCore;
using game::FoodKind;
using game::MedKind;

struct ShopItem {
    const char* name;
    int         price;
    enum Kind { Food, Med } kind;
    int item_idx;  // FoodKind or MedKind
};
constexpr int kItemCount = 4;
// 注意：lv_color_hex 不是 constexpr，所以这里用普通 static const
static const ShopItem kItems[kItemCount] = {
    {"零食",   5,  ShopItem::Food, (int)FoodKind::Snack},
    {"高级料", 30, ShopItem::Food, (int)FoodKind::Premium},
    {"最爱食", 80, ShopItem::Food, (int)FoodKind::Favorite},
    {"特效药", 50, ShopItem::Med,  (int)MedKind::Special},
};

static const lv_color_t COL_BG    = lv_color_hex(0xC0DCC0);
static const lv_color_t COL_BAR    = lv_color_hex(0x404040);
static const lv_color_t COL_TEXT  = lv_color_hex(0x202020);
static const lv_color_t COL_WHITE = lv_color_hex(0xFFFFFF);
static const lv_color_t COL_FOCUS = lv_color_hex(0xFF8C00);
static const lv_color_t COL_OK    = lv_color_hex(0x008000);

struct ShopUi {
    lv_obj_t* root          = nullptr;
    lv_obj_t* top_label     = nullptr;     // 顶栏标题
    lv_obj_t* balance_label = nullptr;     // 顶栏右侧余额
    lv_obj_t* item_labels[kItemCount] = {};  // 商品行
    lv_obj_t* hint_label    = nullptr;     // 底栏提示
    int  focus = 0;
    PetCore* pet = nullptr;
    bool want_leave = false;
};
static ShopUi s;

void refresh_balance() {
    if (!s.balance_label) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "$%ld", (long)game::coins_get());
    lv_label_set_text(s.balance_label, buf);
}

void refresh_focus() {
    for (int i = 0; i < kItemCount; ++i) {
        if (!s.item_labels[i]) continue;
        bool focused = (i == s.focus);
        lv_obj_set_style_border_color(s.item_labels[i], focused ? COL_FOCUS : COL_TEXT, 0);
        lv_obj_set_style_border_width(s.item_labels[i], focused ? 3 : 1, 0);
    }
}

void do_buy() {
    const ShopItem& it = kItems[s.focus];
    int32_t price = it.price;
    if (!game::coins_spend(price)) {
        bsp::audio_play(bsp::Sound::Reject);
        if (s.hint_label) lv_label_set_text(s.hint_label, "金币不足");
        return;
    }
    // 应用商品效果：购买 = 清冷却（特效药例外：直接 +1 库存）
    if (s.pet) {
        if (it.kind == ShopItem::Food) {
            // 重置 food_cooldown_pet_sec[item_idx] = 0
            s.pet->clear_food_cooldown((FoodKind)it.item_idx);
        } else {
            // 特效药：直接调用 medicate（库存够即可用，本次算额外一次）
            s.pet->medicate((MedKind)it.item_idx);
        }
    }
    bsp::audio_play(bsp::Sound::Win);
    refresh_balance();
    if (s.hint_label) {
        char buf[24];
        snprintf(buf, sizeof(buf), "购买了 %s", it.name);
        lv_label_set_text(s.hint_label, buf);
    }
}

void on_key(bsp::KeyId id, bsp::KeyEvent evt) {
    if (evt == bsp::KeyEvent::ShortPress) {
        // 必须持 LVGL 锁再改样式/文本：本回调跑在 btn_scan 任务里，
        // 与 LVGL 刷新任务并发操作同一对象会导致内部状态损坏
        // （实测表现：移动一次焦点后整个 UI 冻结、长按退出失效）。
        if (!lvgl_port_lock(100)) return;
        if (id == bsp::KeyId::Left) {
            if (s.focus > 0) s.focus--;
            else s.focus = kItemCount - 1;
            bsp::audio_play(bsp::Sound::Tick);
            refresh_focus();
        } else if (id == bsp::KeyId::Right) {
            s.focus = (s.focus + 1) % kItemCount;
            bsp::audio_play(bsp::Sound::Tick);
            refresh_focus();
        } else if (id == bsp::KeyId::Mid) {
            do_buy();
        }
        lvgl_port_unlock();
    } else if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        s.want_leave = true;
    }
}

}  // namespace

lv_obj_t* ui_shop_create() {
    s.root = lv_obj_create(nullptr);
    lv_obj_set_size(s.root, 240, 240);
    lv_obj_set_style_bg_color(s.root, COL_BG, 0);
    lv_obj_set_style_bg_opa(s.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s.root, LV_OBJ_FLAG_SCROLLABLE);

    // 顶栏
    lv_obj_t* top = lv_obj_create(s.root);
    lv_obj_set_size(top, 240, 24);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, COL_BAR, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    s.top_label = lv_label_create(top);
    lv_label_set_text(s.top_label, "商店");
    lv_obj_set_style_text_color(s.top_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s.top_label, ui_font_16, 0);
    lv_obj_align(s.top_label, LV_ALIGN_LEFT_MID, 8, 0);
    s.balance_label = lv_label_create(top);
    refresh_balance();
    lv_obj_set_style_text_color(s.balance_label, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(s.balance_label, ui_font_16, 0);
    lv_obj_align(s.balance_label, LV_ALIGN_RIGHT_MID, -8, 0);

    // 商品列表（4 行，y=40 起每行 36px 高）
    for (int i = 0; i < kItemCount; ++i) {
        lv_obj_t* row = lv_obj_create(s.root);
        lv_obj_set_size(row, 216, 32);
        lv_obj_set_pos(row, 12, 40 + i * 36);
        lv_obj_set_style_bg_color(row, COL_WHITE, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, COL_TEXT, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* name_lbl = lv_label_create(row);
        char buf[32];
        snprintf(buf, sizeof(buf), "%s", kItems[i].name);
        lv_label_set_text(name_lbl, buf);
        lv_obj_set_style_text_color(name_lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(name_lbl, ui_font_16, 0);
        lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 8, 0);
        // 价格
        lv_obj_t* price_lbl = lv_label_create(row);
        snprintf(buf, sizeof(buf), "$%d", kItems[i].price);
        lv_label_set_text(price_lbl, buf);
        lv_obj_set_style_text_color(price_lbl, lv_color_hex(0xFFD700), 0);
        lv_obj_set_style_text_font(price_lbl, ui_font_16, 0);
        lv_obj_align(price_lbl, LV_ALIGN_RIGHT_MID, -8, 0);
        s.item_labels[i] = row;
    }

    // 底栏
    lv_obj_t* bot = lv_obj_create(s.root);
    lv_obj_set_size(bot, 240, 24);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_style_bg_color(bot, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    s.hint_label = lv_label_create(bot);
    lv_label_set_text(s.hint_label, "左/右选 中键买 长按退");
    lv_obj_set_style_text_color(s.hint_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s.hint_label, ui_font_16, 0);
    lv_obj_center(s.hint_label);

    s.focus = 0;
    s.want_leave = false;
    refresh_focus();
    bsp::buttons_set_callback(on_key);
    return s.root;
}

void ui_shop_set_pet(::boxpet::game::PetCore* pet) { s.pet = pet; }

// 商店想退出（外部主循环查询用）
bool ui_shop_wants_to_leave() { return s.want_leave; }
void ui_shop_clear_leave_flag() { s.want_leave = false; }

// 关闭时清理
void ui_shop_close() {
    if (s.root && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.root);
        lvgl_port_unlock();
    }
    s.root = nullptr;
}

}  // namespace boxpet::ui