// ui_settings.cpp — 设置菜单（中键长按进入；左右切换焦点项；中键确认/编辑）
// 六项：
//   1. 时间模式（演示/真实）
//   2. 音效（开/关）
//   3. 小时
//   4. 分钟
//   5. 相亲（繁育，需求 §4：Lv15+成熟期）
//   6. 重置存档
// 时间项交互（编辑模式）：
//   中键短按 → 进入编辑；编辑中 左=-1 / 右=+1 / 中键=完成
//   时间项上中键长按 → 进入编辑（不退出，防止按住调时间误触"长按退出"）
//   仅在非时间项上中键长按 → 退出设置
#include "ui_settings.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/audio.h"
#include "bsp/wallclock.h"
#include "game/storage.h"
#include "game/pet_def.h"
#include "esp_log.h"
#include <cstdio>

static const char* TAG = "ui_settings";

namespace boxpet::ui {

namespace {

constexpr int kItems = 6;
struct Item {
    const char* name;
    int         y_pos;
    lv_obj_t*   label;     // 显示当前值的标签
    lv_obj_t*   cursor;    // 左侧 > 箭头
};
struct State {
    lv_obj_t* root = nullptr;
    Item      items[kItems];
    int       sel = 0;
    bool      editing = false;           // 时间项编辑模式（小时/分钟）
    lv_obj_t* hint = nullptr;            // 底部提示条
    ::boxpet::game::PetCore* pet = nullptr;
    bool      wants_leave = false;
    bool      wants_reset  = false;
    const char* breed_result = nullptr;  // 最近一次相亲结果
};
static State s;

static const lv_color_t COL_BG   = lv_color_hex(0xC0DCC0);
static const lv_color_t COL_TEXT = lv_color_hex(0x202020);
static const lv_color_t COL_BAR  = lv_color_hex(0x404040);
static const lv_color_t COL_WHITE = lv_color_hex(0xFFFFFF);
static const lv_color_t COL_CURSOR = lv_color_hex(0x008000);

static const char* breed_fail_text(int why) {
    switch (why) {
        case 0: return "相亲：等级不够";
        case 1: return "相亲：尚未成熟";
        case 2: return "相亲：次数用尽";
        case 3: return "相亲：孕育中";
        case 4: return "相亲：状态不佳";
        case 6: return "相亲：冷却中";
        default: return "相亲：暂不可";
    }
}

static void refresh() {
    using namespace boxpet::game;
    if (!s.pet) return;
    if (!lvgl_port_lock(100)) return;  // 可能从按键扫描任务调用，必须持锁
    const PetState& st = s.pet->state();
    char buf[32];
    snprintf(buf, sizeof(buf), "时间模式 %s", st.time_mode == TimeMode::Real ? "真实" : "演示");
    lv_label_set_text(s.items[0].label, buf);
    snprintf(buf, sizeof(buf), "音效 %s", bsp::audio_is_muted() ? "关" : "开");
    lv_label_set_text(s.items[1].label, buf);
    int h, m, sec;
    bsp::wallclock_now(&h, &m, &sec);
    snprintf(buf, sizeof(buf), "小时 %02d", h);
    lv_label_set_text(s.items[2].label, buf);
    snprintf(buf, sizeof(buf), "分钟 %02d", m);
    lv_label_set_text(s.items[3].label, buf);
    // 相亲项：最近结果 / 常规提示
    if (s.breed_result) {
        lv_label_set_text(s.items[4].label, s.breed_result);
    } else if (st.gestation_end_pet_sec > 0) {
        lv_label_set_text(s.items[4].label, "相亲：孕育中");
    } else {
        lv_label_set_text(s.items[4].label, "相亲（繁育）");
    }
    lv_label_set_text(s.items[5].label, "重置存档");
    // 光标 + 编辑态高亮
    for (int i = 0; i < kItems; ++i) {
        if (s.items[i].cursor) {
            if (i == s.sel) {
                lv_obj_clear_flag(s.items[i].cursor, LV_OBJ_FLAG_HIDDEN);
                lv_label_set_text(s.items[i].cursor, s.editing ? "*" : ">");
            }
            else lv_obj_add_flag(s.items[i].cursor, LV_OBJ_FLAG_HIDDEN);
        }
        if (s.items[i].label) {
            // 编辑中的时间项：橙色；其余：常规黑
            bool hot = s.editing && (i == s.sel) && (i == 2 || i == 3);
            lv_obj_set_style_text_color(s.items[i].label,
                hot ? lv_color_hex(0xE06000) : COL_TEXT, 0);
        }
    }
    // 底部提示
    if (s.hint) {
        lv_label_set_text(s.hint, s.editing ? "左:- 右:+ 中:完成"
                                            : "中：编辑  长按：返回");
    }
    lvgl_port_unlock();
}

// 编辑模式中调整时间（delta = ±1）
static void adjust_time(int delta) {
    int h, m, sec;
    bsp::wallclock_now(&h, &m, &sec);
    if (s.sel == 2) h = (int)(((h + delta) % 24 + 24) % 24);
    else            m = (int)(((m + delta) % 60 + 60) % 60);
    bsp::wallclock_set(h, m);
    bsp::audio_play(bsp::Sound::Tick);
}

static void on_key(bsp::KeyId id, bsp::KeyEvent evt) {
    using namespace boxpet::game;
    if (evt == bsp::KeyEvent::ShortPress) {
        if (id == bsp::KeyId::Left) {
            if (s.editing) adjust_time(-1);
            else {
                s.sel = (s.sel + kItems - 1) % kItems;
                bsp::audio_play(bsp::Sound::Tick);   // 切换项：轻 tick
            }
            refresh();
        } else if (id == bsp::KeyId::Right) {
            if (s.editing) adjust_time(+1);
            else {
                s.sel = (s.sel + 1) % kItems;
                bsp::audio_play(bsp::Sound::Tick);
            }
            refresh();
        } else if (id == bsp::KeyId::Mid) {
            if (s.editing) {
                // 完成编辑
                s.editing = false;
                bsp::audio_play(bsp::Sound::Beep);
            } else if (s.sel == 0 && s.pet) {
                // 时间模式
                PetState st = s.pet->state();
                st.time_mode = (st.time_mode == TimeMode::Real) ? TimeMode::Demo : TimeMode::Real;
                s.pet->load_state(st);
            } else if (s.sel == 1) {
                bsp::audio_set_muted(!bsp::audio_is_muted());
            } else if (s.sel == 2 || s.sel == 3) {
                // 进入时间编辑模式
                s.editing = true;
                bsp::audio_play(bsp::Sound::Beep);
            } else if (s.sel == 4 && s.pet) {
                // 相亲（繁育，需求 §4）
                s.breed_result = nullptr;
                int why = 0;
                if (!s.pet->can_breed(&why)) {
                    s.breed_result = breed_fail_text(why);
                    bsp::audio_play(bsp::Sound::Reject);
                } else {
                    s.pet->breed_attempt();
                    // 结果判定：孕育开始 = 成功
                    if (s.pet->state().gestation_end_pet_sec > 0) {
                        s.breed_result = "相亲：有宝宝了！";
                        bsp::audio_play(bsp::Sound::Evolve);
                    } else {
                        s.breed_result = "相亲：没成功…";
                        bsp::audio_play(bsp::Sound::Reject);
                    }
                }
            } else if (s.sel == 5) {
                s.wants_reset = true;
            }
            refresh();
        }
    } else if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        // 修复 bug：时间项上按住中键调时间不再被误判为"长按退出"
        if (s.editing) {
            // 编辑模式中长按：忽略（不退出）
        } else if (s.sel == 2 || s.sel == 3) {
            // 时间项上长按：进入编辑模式（用户意图是调整时间）
            s.editing = true;
            refresh();
        } else {
            s.wants_leave = true;
        }
    }
}

static lv_obj_t* build() {
    lv_obj_t* root = lv_obj_create(nullptr);
    lv_obj_set_size(root, 240, 240);
    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* top = lv_obj_create(root);
    lv_obj_set_size(top, 240, 24);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, COL_BAR, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, COL_WHITE, 0);
    lv_obj_set_style_text_font(title, ui_font_16, 0);
    lv_obj_center(title);
    int ys[kItems] = {42, 68, 94, 120, 146, 172};
    for (int i = 0; i < kItems; ++i) {
        // 左侧 >
        s.items[i].cursor = lv_label_create(root);
        lv_label_set_text(s.items[i].cursor, ">");
        lv_obj_set_pos(s.items[i].cursor, 30, ys[i]);
        lv_obj_set_style_text_color(s.items[i].cursor, COL_CURSOR, 0);
        lv_obj_set_style_text_font(s.items[i].cursor, ui_font_16, 0);
        lv_obj_add_flag(s.items[i].cursor, LV_OBJ_FLAG_HIDDEN);
        s.items[i].y_pos = ys[i];
        s.items[i].label = lv_label_create(root);
        lv_label_set_text(s.items[i].label, "");
        lv_obj_set_pos(s.items[i].label, 60, ys[i]);
        lv_obj_set_style_text_color(s.items[i].label, COL_TEXT, 0);
        lv_obj_set_style_text_font(s.items[i].label, ui_font_16, 0);
    }
    lv_obj_t* bot = lv_obj_create(root);
    lv_obj_set_size(bot, 240, 24);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_style_bg_color(bot, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* hint = lv_label_create(bot);
    lv_label_set_text(hint, "中：编辑  长按：返回");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_set_style_text_font(hint, ui_font_16, 0);
    lv_obj_center(hint);
    s.hint = hint;
    return root;
}

}  // namespace

lv_obj_t* ui_settings_create() {
    s.root = build();
    s.sel = 0;
    s.editing = false;
    s.breed_result = nullptr;
    bsp::buttons_set_callback(on_key);
    refresh();
    return s.root;
}

void ui_settings_set_pet(::boxpet::game::PetCore* pet) {
    s.pet = pet;
    refresh();
}

void ui_settings_close() {
    // 按键回调由 main.cpp 在退出时通过 ui_main_attach_key(nullptr) 恢复
    // 删除屏幕对象（修复泄漏，见 ui_status_close 注释）
    if (s.root && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.root);
        lvgl_port_unlock();
    }
    s.root = nullptr;
    s.pet = nullptr;
    s.editing = false;
}

bool ui_settings_wants_leave() { return s.wants_leave; }
void ui_settings_clear_leave_flag() { s.wants_leave = false; }
bool ui_settings_wants_reset() { return s.wants_reset; }
void ui_settings_clear_reset_flag() { s.wants_reset = false; }

}  // namespace boxpet::ui