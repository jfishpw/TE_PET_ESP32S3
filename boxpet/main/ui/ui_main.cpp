// ui/ui_main.cpp — 主界面（需求 v2）
//  顶栏 24px（电池 / 时钟 / 注意图标）
//  上图标行 40px（食/光/玩/药）
//  中间宠物区 92px + 草地（含模态菜单 / 事件弹窗浮层）
//  下图标行 40px（清/状/教/摸）
//  图标语义：食=喂食菜单 光=开关灯 玩=玩耍菜单 药=药品菜单
//            清=洗澡 状=状态页 教=教育菜单 摸=抚摸
#include "ui_main.h"
#include "ui_font_16.h"
#include "lvgl_sprite.h"
#include "anim.h"
#include "bsp/board.h"
#include "bsp/audio.h"
#include "bsp/wallclock.h"
#include "bsp/power.h"
#include "bsp/power_mgr.h"
#include "game/pet_def.h"
#include "game/pet_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cmath>

static const char* TAG = "ui_main";

namespace boxpet::ui {

namespace {

using game::PetCore;
using game::Event;
using game::EventKind;
using game::FoodKind;
using game::MedKind;
using game::PlayKind;
using game::EduKind;
using game::SpecialEventId;

struct IconDesc {
    const char* label;
    bool        selectable;
};

static const lv_color_t COL_BG          = lv_color_hex(0xC0DCC0);
static const lv_color_t COL_BAR         = lv_color_hex(0x303030);
static const lv_color_t COL_TEXT        = lv_color_hex(0x202020);
static const lv_color_t COL_TEXT_BAR    = lv_color_hex(0xFFFFFF);
static const lv_color_t COL_ICON_NORMAL = lv_color_hex(0x202020);
static const lv_color_t COL_ICON_FOCUS  = lv_color_hex(0xFF8C00);   // 焦点：橙色底
static const lv_color_t COL_ICON_BG     = lv_color_hex(0x505A50);
// 宠物区背景：天空 + 草地（全彩屏）
static const lv_color_t COL_SKY         = lv_color_hex(0xBFE3F5);
static const lv_color_t COL_GRASS       = lv_color_hex(0x8CD08C);
static const lv_color_t COL_PANEL       = lv_color_hex(0xF5F0DC);

static const IconDesc kIcons[8] = {
    {"食",  true},
    {"光",  true},
    {"玩",  true},
    {"药",  true},
    {"清",  true},
    {"状",  true},
    {"教",  true},
    {"摸",  true},
};

// ===== 模态菜单 =====
enum class MenuMode : uint8_t { None = 0, Food, Med, Play, Edu };

struct UiState {
    lv_obj_t* root        = nullptr;
    lv_obj_t* top_bar     = nullptr;
    lv_obj_t* batt_label  = nullptr;
    lv_obj_t* clock_label = nullptr;
    lv_obj_t* alert_icon  = nullptr;
    lv_obj_t* icon_objs[8] = {nullptr};
    lv_obj_t* sky_obj     = nullptr;
    lv_obj_t* grass_obj   = nullptr;
    lv_obj_t* pet_canvas  = nullptr;
    lv_obj_t* toast_label = nullptr;
    // 天空装饰：太阳/云（白天），月亮/星星（夜晚）
    lv_obj_t* sun_obj     = nullptr;
    lv_obj_t* cloud1      = nullptr;
    lv_obj_t* cloud2      = nullptr;
    lv_obj_t* moon_obj    = nullptr;
    lv_obj_t* stars[5]    = {nullptr};
    // 模态菜单浮层
    lv_obj_t* menu_panel  = nullptr;
    lv_obj_t* menu_title  = nullptr;
    lv_obj_t* menu_item   = nullptr;
    lv_obj_t* menu_hint   = nullptr;
    MenuMode  menu        = MenuMode::None;
    int       menu_sel    = 0;
    // 特殊事件弹窗
    lv_obj_t* ev_panel    = nullptr;
    lv_obj_t* ev_title    = nullptr;
    lv_obj_t* ev_desc     = nullptr;
    lv_obj_t* ev_choices  = nullptr;
    lv_obj_t* ev_hint     = nullptr;
    bool      ev_visible  = false;
    int64_t   toast_until_ms = 0;
    int       focus       = 0;
    bsp::KeyCallback user_cb = nullptr;
    PetCore*  pet          = nullptr;
    SpriteAnimator anim;
    int       attn_bits    = 0;
    bool      user_wants_game   = false;
    bool      user_wants_status = false;
    bool      user_wants_settings = false;
    bool      user_wants_resurrect = false;
    // 进入游戏场景的模式
    bool      pending_is_edu = false;
    uint8_t   pending_kind   = 0;
};

static UiState g;
static esp_timer_handle_t g_tick_timer = nullptr;

static lv_obj_t* make_label(lv_obj_t* parent, const char* text,
                            lv_color_t color, uint8_t pct) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, ui_font_16, 0);
    lv_obj_set_width(l, LV_PCT(pct));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

static lv_obj_t* make_icon(lv_obj_t* parent, const char* text,
                           lv_color_t bg) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, 48, 40);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, COL_ICON_NORMAL, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_radius(o, 4, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* lbl = lv_label_create(o);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, ui_font_16, 0);
    lv_obj_center(lbl);
    return o;
}

static void apply_focus(bool focused) {
    if (g.focus < 0 || g.focus >= 8) return;
    lv_obj_t* o = g.icon_objs[g.focus];
    if (!o) return;
    if (focused) {
        lv_obj_set_style_bg_color(o, COL_ICON_FOCUS, 0);
        lv_obj_t* lbl = lv_obj_get_child(o, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, COL_TEXT_BAR, 0);
    } else {
        bool selectable = kIcons[g.focus].selectable;
        lv_obj_set_style_bg_color(o, selectable ? COL_ICON_BG : COL_BAR, 0);
        lv_obj_t* lbl = lv_obj_get_child(o, 0);
        if (lbl) lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    }
}

static void focus_move(int delta) {
    apply_focus(false);
    int idx = g.focus + delta;
    do { idx = (idx + 8) % 8; } while (!kIcons[idx].selectable);
    g.focus = idx;
    apply_focus(true);
    bsp::audio_play(bsp::Sound::Tick);
}

// toast：宠物区顶部短暂提示（操作可见反馈）
// 注意：可能从 esp_timer 任务（pet 事件）调用，内部持锁；递归锁支持重入。
static void show_toast(const char* text, int duration_ms = 2000) {
    if (!g.toast_label) return;
    if (!lvgl_port_lock(200)) return;
    lv_label_set_text(g.toast_label, text);
    lv_obj_clear_flag(g.toast_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g.toast_label);
    g.toast_until_ms = esp_timer_get_time() / 1000 + duration_ms;
    lvgl_port_unlock();
}

// 昼夜切换：白天=浅天蓝+太阳云朵，夜晚=深蓝+月亮星星
// 调用需持 LVGL 锁
static void apply_day_night(bool light_on) {
    lv_color_t sky    = light_on ? lv_color_hex(0xBFE3F5) : lv_color_hex(0x25315F);
    lv_color_t grass  = light_on ? lv_color_hex(0x8CD08C) : lv_color_hex(0x2C4A2C);
    if (g.sky_obj)   lv_obj_set_style_bg_color(g.sky_obj, sky, 0);
    if (g.grass_obj) lv_obj_set_style_bg_color(g.grass_obj, grass, 0);
    // 装饰可见性
    auto vis = [](lv_obj_t* o, bool show) {
        if (!o) return;
        if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        else      lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    };
    vis(g.sun_obj, light_on);
    vis(g.cloud1, light_on);
    vis(g.cloud2, light_on);
    vis(g.moon_obj, !light_on);
    for (auto& st : g.stars) vis(st, !light_on);
}

// ===== 模态菜单 =====
static int menu_item_count() {
    switch (g.menu) {
        case MenuMode::Food: return (int)FoodKind::Count;
        case MenuMode::Med:  return (int)MedKind::Count;
        case MenuMode::Play: return (int)PlayKind::Count;
        case MenuMode::Edu:  return (int)EduKind::Count;
        default: return 0;
    }
}

// 调用需持 LVGL 锁
static void menu_refresh_locked() {
    if (g.menu == MenuMode::None || !g.pet) return;
    char buf[48];
    const char* title = "";
    const auto& st = g.pet->state();
    switch (g.menu) {
        case MenuMode::Food: {
            title = "选食物";
            int k = g.menu_sel;
            if (game::kFoodInfinite[k])
                snprintf(buf, sizeof(buf), "%s 无限", game::kFoods[k].name);
            else
                snprintf(buf, sizeof(buf), "%s x%d", game::kFoods[k].name,
                         (int)st.food_inv[k]);
            break;
        }
        case MenuMode::Med: {
            title = "选药品";
            int k = g.menu_sel;
            if (game::kMedInfinite[k])
                snprintf(buf, sizeof(buf), "%s 无限", game::kMeds[k].name);
            else
                snprintf(buf, sizeof(buf), "%s x%d", game::kMeds[k].name,
                         (int)st.med_inv[k]);
            break;
        }
        case MenuMode::Play: {
            title = "选玩耍";
            int k = g.menu_sel;
            if (st.level < game::kPlays[k].unlock_level)
                snprintf(buf, sizeof(buf), "%s Lv%d解锁", game::kPlays[k].name,
                         game::kPlays[k].unlock_level);
            else
                snprintf(buf, sizeof(buf), "%s", game::kPlays[k].name);
            break;
        }
        case MenuMode::Edu: {
            title = "选课程";
            int k = g.menu_sel;
            if (st.level < game::kEdus[k].unlock_level)
                snprintf(buf, sizeof(buf), "%s Lv%d解锁", game::kEdus[k].name,
                         game::kEdus[k].unlock_level);
            else
                snprintf(buf, sizeof(buf), "%s", game::kEdus[k].name);
            break;
        }
        default: buf[0] = 0; break;
    }
    if (g.menu_title) lv_label_set_text(g.menu_title, title);
    if (g.menu_item)  lv_label_set_text(g.menu_item, buf);
}

// 打开菜单（内部持锁）
static void menu_open(MenuMode m) {
    if (!lvgl_port_lock(200)) return;
    g.menu = m;
    g.menu_sel = 0;
    if (g.menu_panel) {
        lv_obj_clear_flag(g.menu_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g.menu_panel);
    }
    menu_refresh_locked();
    lvgl_port_unlock();
    bsp::audio_play(bsp::Sound::Beep);
}

// 关闭菜单（调用需持 LVGL 锁）
static void menu_close_locked() {
    g.menu = MenuMode::None;
    if (g.menu_panel) lv_obj_add_flag(g.menu_panel, LV_OBJ_FLAG_HIDDEN);
}

// ===== 特殊事件弹窗（需求 §5.4）=====
struct EvText {
    const char* title;
    const char* desc;
    const char* left;    // 左键选项
    const char* right;   // 右键选项
};

static EvText ev_text(SpecialEventId id) {
    switch (id) {
        case SpecialEventId::Visitor:   return {"访客来了", "小伙伴来串门啦", "欢迎", "赶走"};
        case SpecialEventId::Rain:      return {"下雨了", "窗外下起了雨", "出去玩", "在家待"};
        case SpecialEventId::Nightmare: return {"做噩梦", "宝宝被吓醒了", "安慰", "不管"};
        case SpecialEventId::Meteor:    return {"流星", "流星划过夜空", "许愿", "作罢"};
        case SpecialEventId::Merchant:  return {"神秘商人", "斗篷商人来交易", "交易", "拒绝"};
        case SpecialEventId::Runaway:   return {"离家出走", "它收拾了小包袱", "追回", "随它去"};
        default:                        return {"事件", "……", "好的", "不了"};
    }
}

// 显示事件弹窗（内部持锁；pet tick 上下文调用）
static void show_event_popup(SpecialEventId id) {
    if (!lvgl_port_lock(200)) return;
    EvText t = ev_text(id);
    if (g.ev_title)   lv_label_set_text(g.ev_title, t.title);
    if (g.ev_desc)    lv_label_set_text(g.ev_desc, t.desc);
    if (g.ev_choices) {
        char buf[48];
        snprintf(buf, sizeof(buf), "左键:%s  右键:%s", t.left, t.right);
        lv_label_set_text(g.ev_choices, buf);
    }
    if (g.ev_panel) {
        lv_obj_clear_flag(g.ev_panel, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(g.ev_panel);
    }
    g.ev_visible = true;
    lvgl_port_unlock();
}

// 隐藏事件弹窗（内部持锁）
static void hide_event_popup() {
    if (!lvgl_port_lock(200)) return;
    g.ev_visible = false;
    if (g.ev_panel) lv_obj_add_flag(g.ev_panel, LV_OBJ_FLAG_HIDDEN);
    lvgl_port_unlock();
}

// ===== 图标确认 =====
static const char* feed_reject_text(int why) {
    switch (why) {
        case 0: return "已经吃饱啦";
        case 1: return "睡着了…";
        case 2: return "没心情吃…";
        case 3: return "这个刚吃过";
        case 4: return "没有库存了";
        default: return "蛋宝宝不能喂";
    }
}

static const char* play_reject_text(int why) {
    switch (why) {
        case 0: return "等级还不够";
        case 1: return "睡着了…";
        case 2: return "没心情玩…";
        case 3: return "没力气了…";
        case 4: return "今天玩够啦";
        case 5: return "生病不能玩";
        default: return "现在不能玩";
    }
}

static const char* edu_reject_text(int why) {
    switch (why) {
        case 0: return "等级还不够";
        case 1: return "睡着了…";
        case 2: return "没心情学…";
        case 3: return "学不动了…";
        case 4: return "今天学完啦";
        case 5: return "生病不能学";
        default: return "现在不能学";
    }
}

static void confirm_focus() {
    if (!g.pet) return;
    const auto& st = g.pet->state();
    bool egg  = (st.stage == game::Stage::Egg);
    bool dead = (st.pstate == game::PetStateKind::DEAD);
    // 蛋/死亡：仅状态页可看
    if ((egg || dead) && g.focus != 5) {
        show_toast(egg ? "蛋宝宝孵化中…" : "它安息了…");
        bsp::audio_play(bsp::Sound::Reject);
        return;
    }
    switch (g.focus) {
        case 0: menu_open(MenuMode::Food); break;    // 食
        case 1: g.pet->toggle_light(); break;        // 光
        case 2: menu_open(MenuMode::Play); break;    // 玩
        case 3: menu_open(MenuMode::Med); break;     // 药
        case 4: g.pet->bathe(); break;               // 清
        case 5:                                     // 状
            g.user_wants_status = true;
            bsp::audio_play(bsp::Sound::Beep);
            break;
        case 6: menu_open(MenuMode::Edu); break;     // 教
        case 7: g.pet->pet_touch(); break;           // 摸
    }
}

// 菜单确认（调用需持 LVGL 锁）
static void menu_confirm_locked() {
    if (!g.pet) return;
    int k = g.menu_sel;
    switch (g.menu) {
        case MenuMode::Food:
            menu_close_locked();
            g.pet->feed((FoodKind)k);
            break;
        case MenuMode::Med:
            menu_close_locked();
            g.pet->medicate((MedKind)k);
            break;
        case MenuMode::Play: {
            int why = 0;
            if (!g.pet->can_play((PlayKind)k, &why)) {
                show_toast(play_reject_text(why));
                bsp::audio_play(bsp::Sound::Reject);
                return;
            }
            if ((PlayKind)k == PlayKind::Free) {
                // 自由玩耍：无需小游戏，直接结算
                menu_close_locked();
                g.pet->play_begin(PlayKind::Free);
                g.pet->play_end(PlayKind::Free, true);
            } else {
                menu_close_locked();
                g.pending_is_edu = false;
                g.pending_kind = (uint8_t)k;
                g.user_wants_game = true;
                bsp::audio_play(bsp::Sound::Beep);
            }
            break;
        }
        case MenuMode::Edu: {
            int why = 0;
            if (!g.pet->can_learn((EduKind)k, &why)) {
                show_toast(edu_reject_text(why));
                bsp::audio_play(bsp::Sound::Reject);
                return;
            }
            menu_close_locked();
            g.pending_is_edu = true;
            g.pending_kind = (uint8_t)k;
            g.user_wants_game = true;
            bsp::audio_play(bsp::Sound::Beep);
            break;
        }
        default: break;
    }
}

// ===== 空闲自主行为（无操作时宠物自己活动）=====
namespace {
int64_t s_next_idle_ms  = 0;     // 下次空闲动作时刻（0 = 待定，按键后重置）
int     s_wander_target = 0;     // 漫步目标偏移（屏幕 px）
int     s_wander_cur    = 0;     // 当前漫步偏移
int     s_jump_left_ms  = 0;     // 跳跃剩余时间
}

static void idle_behavior_tick(int64_t now_ms) {
    if (s_next_idle_ms == 0) {
        s_next_idle_ms = now_ms + 8000 + esp_random() % 8000;   // 8~16s 后第一个动作
    }
    // 跳跃动画：600ms 正弦弧线
    if (s_jump_left_ms > 0) s_jump_left_ms -= 100;
    // 漫步缓动
    s_wander_cur += (s_wander_target - s_wander_cur) / 3;
    if (s_wander_target == 0 && s_wander_cur > -1 && s_wander_cur < 1) s_wander_cur = 0;

    if (now_ms < s_next_idle_ms) return;
    // 到点：随机挑一个动作
    uint32_t r = esp_random() % 5;
    switch (r) {
        case 0: g.anim.trigger(AnimAction::Happy, 900); break;          // 开心一下
        case 1: s_wander_target = 12 + (int)(esp_random() % 10); break; // 向右逛
        case 2: s_wander_target = -(12 + (int)(esp_random() % 10)); break; // 向左逛
        case 3: s_jump_left_ms = 600; break;                            // 跳一下
        default: s_wander_target = 0; break;                            // 踱回中间
    }
    s_next_idle_ms = now_ms + 6000 + esp_random() % 9000;               // 6~15s 后再来
}

static int idle_jump_y_off() {
    if (s_jump_left_ms <= 0) return 0;
    float p = 1.0f - (float)s_jump_left_ms / 600.0f;
    return (int)(-(sinf((float)M_PI * p) * 12.0f));
}

static void on_key(bsp::KeyId id, bsp::KeyEvent evt) {
    s_next_idle_ms  = 0;                              // 任何按键重置空闲计时
    s_wander_target = 0;                              // 操作时先站回中间
    if (evt == bsp::KeyEvent::ShortPress) {
        // 按键扫描任务上下文：LVGL 操作必须持锁
        if (!lvgl_port_lock(100)) return;
        if (g.ev_visible && g.pet) {
            // 事件弹窗：左=选项0 右=选项1 中=选项0（默认）
            int choice = (id == bsp::KeyId::Right) ? 1 : 0;
            lvgl_port_unlock();
            g.pet->resolve_event(choice);
            return;
        }
        if (g.menu != MenuMode::None) {
            int n = menu_item_count();
            if (id == bsp::KeyId::Left) {
                g.menu_sel = (g.menu_sel + n - 1) % n;
                bsp::audio_play(bsp::Sound::Tick);
                menu_refresh_locked();
            } else if (id == bsp::KeyId::Right) {
                g.menu_sel = (g.menu_sel + 1) % n;
                bsp::audio_play(bsp::Sound::Tick);
                menu_refresh_locked();
            } else if (id == bsp::KeyId::Mid) {
                menu_confirm_locked();
            }
            lvgl_port_unlock();
            return;
        }
        if (id == bsp::KeyId::Left)  focus_move(-1);
        if (id == bsp::KeyId::Right) focus_move( 1);
        if (id == bsp::KeyId::Mid)   confirm_focus();
        lvgl_port_unlock();
    } else if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        if (g.menu != MenuMode::None) {
            if (lvgl_port_lock(100)) {
                menu_close_locked();
                lvgl_port_unlock();
            }
            bsp::audio_play(bsp::Sound::Reject);
            return;
        }
        // 死亡时长按中键 = 孵化新蛋；否则进入设置
        if (g.pet && g.pet->state().pstate == game::PetStateKind::DEAD) {
            g.user_wants_resurrect = true;
        } else {
            g.user_wants_settings = true;
        }
    }
    if (g.user_cb) g.user_cb(id, evt);
}

// 订阅 pet 事件，更新 UI + animator
// 关键提醒（特殊事件/死亡/濒死/生病/饥饿/卫生）在熄屏≥30s 时自动亮屏提示；
// 其余状态照常后台推进，亮屏瞬间由 tick_timer 统一刷新显示。
static void wake_alert() { bsp::power_mgr_wake_for_alert(); }

static void on_pet_event(const Event& e) {
    using K = EventKind;
    switch (e.kind) {
        case K::StageChanged:
            ESP_LOGI(TAG, "StageChanged → v1=%d", e.v1);
            // 蛋破壳 → 孵化音；其他阶段跃迁 → 进化音
            bsp::audio_play(e.v1 == (int)game::Stage::Baby ? bsp::Sound::Hatch
                                                           : bsp::Sound::Evolve);
            g.anim.trigger(AnimAction::Happy, 1500);
            show_toast(e.v1 == (int)game::Stage::Baby ? "破壳而出！"
                                                      : "长大了！");
            break;
        case K::EvoDecided: {
            const char* form = "";
            switch ((game::EvoForm)e.v1) {
                case game::EvoForm::Scholar:  form = "学者型"; break;
                case game::EvoForm::Active:   form = "活力型"; break;
                case game::EvoForm::Graceful: form = "优雅型"; break;
                case game::EvoForm::Radiant:  form = "光辉型"; break;
                default:                      form = "普通型"; break;
            }
            show_toast(form);
            break;
        }
        case K::Sick:
            g.anim.trigger(AnimAction::Sick, 0);  // 持续（状态帧接管）
            show_toast("生病了，快喂药");
            bsp::audio_play(bsp::Sound::Call);
            wake_alert();                          // 生病提醒亮屏
            break;
        case K::Healed:
            g.anim.trigger(AnimAction::Happy, 1200);
            show_toast("药到病除！");
            bsp::audio_play(bsp::Sound::Heal);
            break;
        case K::Died:
            g.anim.trigger(AnimAction::Died, 0);
            show_toast("它安息了…");
            bsp::audio_play(bsp::Sound::Die);
            wake_alert();                          // 死亡提醒亮屏
            break;
        case K::Dying:
            show_toast("非常危险！！");
            bsp::audio_play(bsp::Sound::Call);
            wake_alert();                          // 濒死提醒亮屏
            break;
        case K::FeedOk:
            g.anim.trigger(AnimAction::Feed, 1600);
            show_toast(e.v1 == (int)FoodKind::Snack ? "零食真香～" : "开饭啦～");
            bsp::audio_play(bsp::Sound::Feed);
            break;
        case K::FeedRejected:
            show_toast(feed_reject_text(e.v1));
            bsp::audio_play(bsp::Sound::Reject);
            break;
        case K::LightToggled:
            if (lvgl_port_lock(200)) {
                apply_day_night(e.v1 != 0);
                lvgl_port_unlock();
            }
            break;
        case K::SleepStart:
            show_toast("晚安～");
            bsp::audio_play(bsp::Sound::Sleep);
            break;
        case K::WakeUp:
            show_toast(e.v1 == 3 ? "被吵醒了，生气" : "睡醒啦！");
            if (e.v1 == 3) bsp::audio_play(bsp::Sound::Reject);
            break;
        case K::MedOk:
            // 吃药动画：皱眉摇头（苦）；Healed（退烧/特效）会先触发 Happy
            g.anim.trigger(AnimAction::Med, 1200);
            bsp::audio_play(bsp::Sound::Beep);
            break;
        case K::BatheOk:
            if (e.v1 == 1) {
                show_toast("睡梦中洗不了");
                bsp::audio_play(bsp::Sound::Reject);
            } else {
                g.anim.trigger(AnimAction::Bath, 1600);
                show_toast("冲洗干净！");
                bsp::audio_play(bsp::Sound::Flush);
            }
            break;
        case K::MedRejected: {
            const char* t = "现在不能用药";
            if (e.v1 == 4) t = "没有库存了";
            else if (e.v1 == 1) t = "睡着了…";
            else if (e.v1 == 5) t = "没生病不用退烧药";
            show_toast(t);
            bsp::audio_play(bsp::Sound::Reject);
            break;
        }
        case K::PettedOk:
            if (e.v1 == 1) show_toast("睡得香…");
            else g.anim.trigger(AnimAction::Happy, 1000);
            break;
        case K::Overeat:
            show_toast("吃撑了…");
            bsp::audio_play(bsp::Sound::Reject);
            break;
        case K::Depressed:
            show_toast("情绪低落…多陪陪它");
            bsp::audio_play(bsp::Sound::Call);
            break;
        case K::DepressCured:
            g.anim.trigger(AnimAction::Happy, 1500);
            show_toast("心情好多了！");
            bsp::audio_play(bsp::Sound::Win);
            break;
        case K::PlayFinished:
            show_toast(e.v2 ? "玩得真开心！" : "下次再努力");
            // 时长加长：事件在小游戏场景触发，返回主界面后仍可见
            if (e.v2) g.anim.trigger(AnimAction::Happy, 3000);
            break;
        case K::EduFinished:
            show_toast(e.v2 >= game::kEduQuestions ? "全部答对！"
                                                   : "学习结束");
            if (e.v2 >= game::kEduQuestions) g.anim.trigger(AnimAction::Happy, 3500);
            break;
        case K::SkillLearned:
            show_toast(game::kSkillNames[e.v1]);
            bsp::audio_play(bsp::Sound::Win);
            break;
        case K::LevelUp: {
            char b[32];
            const char* unlock = "";
            switch (e.v2) {
                case 1: unlock = " 解锁零食"; break;
                case 2: unlock = " 解锁认字"; break;
                case 3: unlock = " 解锁捉迷藏"; break;
                case 4: unlock = " 解锁音乐"; break;
                case 5: unlock = " 解锁繁育"; break;
                default: break;
            }
            snprintf(b, sizeof(b), "升级！Lv%d%s", e.v1, unlock);
            show_toast(b);
            bsp::audio_play(bsp::Sound::Evolve);
            break;
        }
        case K::GestationStart:
            show_toast("有宝宝了！");
            bsp::audio_play(bsp::Sound::Evolve);
            break;
        case K::Born:
            g.anim.trigger(AnimAction::Born, 3000);
            show_toast("宝宝出生啦！");
            bsp::audio_play(bsp::Sound::Hatch);
            break;
        case K::AttentionFlash: {
            // 关键位（饿/病/脏/濒死）新出现时亮屏提醒；
            // 心情/睡眠/抑郁等非关键位不亮屏，亮屏刷新时自然可见
            constexpr int kCriticalMask = (1 << 0) | (1 << 2) | (1 << 4) | (1 << 6);
            int new_critical = e.v1 & ~g.attn_bits & kCriticalMask;
            g.attn_bits = e.v1;
            if (new_critical) wake_alert();
            break;
        }
        case K::AutoSleepHint:
            show_toast("该睡觉啦…");
            bsp::audio_play(bsp::Sound::Call);
            break;
        case K::SpecialEvent:
            if ((SpecialEventId)e.v1 == SpecialEventId::Birthday) {
                show_toast("生日快乐！");
                bsp::audio_play(bsp::Sound::Evolve);
            } else {
                show_event_popup((SpecialEventId)e.v1);
                bsp::audio_play(bsp::Sound::Call);
                wake_alert();                      // 特殊事件亮屏（弹窗限时选择）
            }
            break;
        case K::EventResolved: {
            hide_event_popup();
            const char* t = "";
            switch ((SpecialEventId)e.v1) {
                case SpecialEventId::Visitor:
                    t = e.v2 == 0 ? "开心地招待了客人" : "客人走了"; break;
                case SpecialEventId::Rain:
                    t = e.v2 == 0 ? "淋湿了但玩得开心" : "在家看雨"; break;
                case SpecialEventId::Nightmare:
                    t = e.v2 == 0 ? "重新哄睡啦" : "梦到可怕的东西…"; break;
                case SpecialEventId::Meteor:
                    t = e.v2 == 0 ? "愿望会实现的" : "流星飞走了"; break;
                case SpecialEventId::Merchant:
                    t = e.v2 == 0 ? "买到特效药！" : "婉拒了商人"; break;
                case SpecialEventId::Runaway:
                    t = e.v2 == 0 ? "成功追回！" : "它自己会回来的"; break;
                default: break;
            }
            if (t[0]) show_toast(t);
            break;
        }
        case K::GiftReceived: {
            char b[32];
            if (e.v1 == 0) snprintf(b, sizeof(b), "获得%s", game::kFoods[e.v2].name);
            else           snprintf(b, sizeof(b), "获得%s", game::kMeds[e.v2].name);
            show_toast(b);
            bsp::audio_play(bsp::Sound::Win);
            break;
        }
        default:
            break;
    }
}

static void tick_timer_cb(void* /*arg*/) {
    if (!g.clock_label) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    if (!lvgl_port_lock(50)) return;

    // ===== 熄屏节电（需求 §5）=====
    // 熄屏期间跳过一切渲染（时钟/图标闪烁/星星/精灵/空闲行为），只维护 toast
    // 到期标记；游戏逻辑由 pet tick 后台照常推进（否则检测不到提醒时机）。
    // 亮屏瞬间（灭→亮边沿）强制全量刷新显示。
    static bool s_screen_was_off = false;
    static bool s_toast_hide_pending = false;
    static int  s_clock_div = 0;
    if (bsp::power_mgr_is_backlight_off()) {
        if (g.toast_until_ms != 0 && now_ms >= g.toast_until_ms) {
            g.toast_until_ms = 0;
            s_toast_hide_pending = true;   // 唤醒后补一次隐藏
        }
        s_screen_was_off = true;
        lvgl_port_unlock();
        return;
    }
    if (s_screen_was_off) {
        s_screen_was_off = false;
        s_clock_div = 10;                  // 本 tick 立即刷新时钟+电量
        if (s_toast_hide_pending && g.toast_label) {
            lv_obj_add_flag(g.toast_label, LV_OBJ_FLAG_HIDDEN);
            s_toast_hide_pending = false;
        }
    }
    // 真实时钟（wallclock）+ 电量，每 10s 刷新
    {
        if (++s_clock_div >= 10) {
            s_clock_div = 0;
            int h, m, s;
            bsp::wallclock_now(&h, &m, &s);
            char buf[8];
            snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
            lv_label_set_text(g.clock_label, buf);
            // 电量（充电中加"+"，低电量变红）
            bsp::PowerStatus ps = bsp::power_get_status();
            snprintf(buf, sizeof(buf), "%u%%%s", ps.battery_pct,
                     ps.charging ? "+" : "");
            lv_label_set_text(g.batt_label, buf);
            lv_obj_set_style_text_color(g.batt_label,
                ps.low_voltage ? lv_color_hex(0xFF6060) : COL_TEXT_BAR, 0);
        }
    }
    // toast 到期隐藏
    if (g.toast_label && g.toast_until_ms != 0 && now_ms >= g.toast_until_ms) {
        lv_obj_add_flag(g.toast_label, LV_OBJ_FLAG_HIDDEN);
        g.toast_until_ms = 0;
    }
    // 注意图标闪烁（每 600ms 切换）—— 6 次 tick 一次
    {
        static int blink_phase = 0;
        blink_phase = (blink_phase + 1) % 6;
        if (g.attn_bits) {
            lv_label_set_text(g.alert_icon, (blink_phase < 3) ? "!" : " ");
        } else {
            lv_label_set_text(g.alert_icon, " ");
        }
    }
    // 星星闪烁（夜晚）
    if (g.pet && !g.pet->state().light_on) {
        static int star_phase = 0;
        star_phase = (star_phase + 1) % 10;
        for (int i = 0; i < 5; ++i) {
            if (!g.stars[i]) continue;
            int on = ((star_phase + i * 2) % 10) < 7;
            lv_obj_set_style_bg_opa(g.stars[i], on ? LV_OPA_COVER : LV_OPA_30, 0);
        }
    }
    // 精灵帧更新（彩色 4bpp；背景随昼夜）
    if (g.pet_canvas && g.pet) {
        bool changed = false;
        const sprites::Sprite* s = g.anim.tick(now_ms, &changed);
        lv_color_t bg = g.pet->state().light_on ? COL_SKY : lv_color_hex(0x25315F);
        if (s && changed) {
            render_pet_sprite(g.pet_canvas, s, bg);
        }
        // 空闲自主行为：仅 IDLE 且无浮层时
        const auto& st = g.pet->state();
        bool idle_ok = st.pstate == game::PetStateKind::IDLE
                       && g.menu == MenuMode::None && !g.ev_visible
                       && g.anim.current_action() == AnimAction::None;
        if (idle_ok) idle_behavior_tick(now_ms);
        // 呼吸/Zzz 偏移（精灵像素 ×2 = 屏幕像素）+ 漫步/跳跃
        lv_obj_set_pos(g.pet_canvas,
                       (240 - 96) / 2 + g.anim.x_offset() * 2 + s_wander_cur,
                       70 + (124 - 96) / 2 + g.anim.y_offset() * 2 + idle_jump_y_off());
    }
    lvgl_port_unlock();
}

// 浮层面板基座（菜单/事件弹窗共用样式）
static lv_obj_t* make_panel(lv_obj_t* parent, int y, int h) {
    lv_obj_t* p = lv_obj_create(parent);
    lv_obj_set_size(p, 232, h);
    lv_obj_set_pos(p, 4, y);
    lv_obj_set_style_bg_color(p, COL_PANEL, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(p, COL_TEXT, 0);
    lv_obj_set_style_border_width(p, 2, 0);
    lv_obj_set_style_radius(p, 10, 0);
    // 默认主题 padding 非零：子标签坐标会被 padding 平移，
    // 底部文字超出内容区被裁剪（表现为"下半截显示不全"）→ 归零
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(p, LV_OBJ_FLAG_HIDDEN);
    return p;
}

static lv_obj_t* make_panel_label(lv_obj_t* parent, int y, const char* text,
                                  lv_color_t color, lv_text_align_t align) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_pos(l, 0, y);
    lv_obj_set_size(l, 232, 22);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_font(l, ui_font_16, 0);
    lv_obj_set_style_text_align(l, align, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

static lv_obj_t* build_main() {
    g.root = lv_obj_create(nullptr);
    lv_obj_set_size(g.root, 240, 240);
    lv_obj_set_style_bg_color(g.root, COL_BG, 0);
    lv_obj_set_style_bg_opa(g.root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(g.root, LV_OBJ_FLAG_SCROLLABLE);

    // --- 顶栏 ---
    g.top_bar = lv_obj_create(g.root);
    lv_obj_set_size(g.top_bar, 240, 24);
    lv_obj_set_pos(g.top_bar, 0, 0);
    lv_obj_set_style_bg_color(g.top_bar, COL_BAR, 0);
    lv_obj_set_style_bg_opa(g.top_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.top_bar, 0, 0);
    lv_obj_set_style_pad_all(g.top_bar, 0, 0);
    lv_obj_clear_flag(g.top_bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g.top_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g.top_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g.batt_label = make_label(g.top_bar, "100%", COL_TEXT_BAR, 30);
    g.clock_label = make_label(g.top_bar, "00:00", COL_TEXT_BAR, 30);
    g.alert_icon = make_label(g.top_bar, " ", COL_TEXT_BAR, 30);

    // --- 上图标行 ---
    lv_obj_t* top_row = lv_obj_create(g.root);
    lv_obj_set_size(top_row, 240, 40);
    lv_obj_set_pos(top_row, 0, 26);
    lv_obj_set_style_bg_opa(top_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(top_row, 0, 0);
    lv_obj_set_style_pad_all(top_row, 0, 0);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 4; i++) {
        g.icon_objs[i] = make_icon(top_row, kIcons[i].label, COL_ICON_BG);
    }

    // --- 宠物区：天空 + 草地（全彩背景） ---
    g.sky_obj = lv_obj_create(g.root);
    lv_obj_set_size(g.sky_obj, 240, 92);
    lv_obj_set_pos(g.sky_obj, 0, 70);
    lv_obj_set_style_bg_color(g.sky_obj, COL_SKY, 0);
    lv_obj_set_style_bg_opa(g.sky_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.sky_obj, 0, 0);
    lv_obj_set_style_radius(g.sky_obj, 0, 0);
    lv_obj_clear_flag(g.sky_obj, LV_OBJ_FLAG_SCROLLABLE);

    g.grass_obj = lv_obj_create(g.root);
    lv_obj_set_size(g.grass_obj, 240, 32);
    lv_obj_set_pos(g.grass_obj, 0, 162);
    lv_obj_set_style_bg_color(g.grass_obj, COL_GRASS, 0);
    lv_obj_set_style_bg_opa(g.grass_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g.grass_obj, 0, 0);
    lv_obj_set_style_radius(g.grass_obj, 0, 0);
    lv_obj_clear_flag(g.grass_obj, LV_OBJ_FLAG_SCROLLABLE);

    // 精灵画布（叠在天空/草地之上）
    g.pet_canvas = create_pet_canvas(g.root);

    // --- 天空装饰（白天：太阳+云；夜晚：月亮+星星） ---
    g.sun_obj = lv_obj_create(g.root);
    lv_obj_set_size(g.sun_obj, 20, 20);
    lv_obj_set_pos(g.sun_obj, 200, 78);
    lv_obj_set_style_bg_color(g.sun_obj, lv_color_hex(0xFFD54F), 0);
    lv_obj_set_style_bg_opa(g.sun_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.sun_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g.sun_obj, 0, 0);
    lv_obj_clear_flag(g.sun_obj, LV_OBJ_FLAG_SCROLLABLE);

    g.cloud1 = lv_obj_create(g.root);
    lv_obj_set_size(g.cloud1, 34, 12);
    lv_obj_set_pos(g.cloud1, 18, 92);
    lv_obj_set_style_bg_color(g.cloud1, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g.cloud1, LV_OPA_90, 0);
    lv_obj_set_style_radius(g.cloud1, 6, 0);
    lv_obj_set_style_border_width(g.cloud1, 0, 0);
    lv_obj_clear_flag(g.cloud1, LV_OBJ_FLAG_SCROLLABLE);

    g.cloud2 = lv_obj_create(g.root);
    lv_obj_set_size(g.cloud2, 26, 10);
    lv_obj_set_pos(g.cloud2, 190, 120);
    lv_obj_set_style_bg_color(g.cloud2, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_bg_opa(g.cloud2, LV_OPA_80, 0);
    lv_obj_set_style_radius(g.cloud2, 5, 0);
    lv_obj_set_style_border_width(g.cloud2, 0, 0);
    lv_obj_clear_flag(g.cloud2, LV_OBJ_FLAG_SCROLLABLE);

    g.moon_obj = lv_obj_create(g.root);
    lv_obj_set_size(g.moon_obj, 16, 16);
    lv_obj_set_pos(g.moon_obj, 202, 80);
    lv_obj_set_style_bg_color(g.moon_obj, lv_color_hex(0xFFF9C4), 0);
    lv_obj_set_style_bg_opa(g.moon_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g.moon_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(g.moon_obj, 0, 0);
    lv_obj_clear_flag(g.moon_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(g.moon_obj, LV_OBJ_FLAG_HIDDEN);

    {
        // 星星（小白点，闪烁由 tick 控制 OPA）
        const int star_xy[5][2] = {{20,84},{60,100},{170,86},{90,78},{210,112}};
        for (int i = 0; i < 5; ++i) {
            g.stars[i] = lv_obj_create(g.root);
            lv_obj_set_size(g.stars[i], 3, 3);
            lv_obj_set_pos(g.stars[i], star_xy[i][0], star_xy[i][1]);
            lv_obj_set_style_bg_color(g.stars[i], lv_color_hex(0xFFFFFF), 0);
            lv_obj_set_style_bg_opa(g.stars[i], LV_OPA_COVER, 0);
            lv_obj_set_style_radius(g.stars[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(g.stars[i], 0, 0);
            lv_obj_add_flag(g.stars[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // toast 提示条（宠物区顶部，默认隐藏）
    g.toast_label = lv_label_create(g.root);
    lv_obj_set_size(g.toast_label, 200, 24);
    lv_obj_set_pos(g.toast_label, 20, 74);
    lv_obj_set_style_bg_color(g.toast_label, lv_color_hex(0x303030), 0);
    lv_obj_set_style_bg_opa(g.toast_label, LV_OPA_80, 0);
    lv_obj_set_style_text_color(g.toast_label, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_text_font(g.toast_label, ui_font_16, 0);
    lv_obj_set_style_text_align(g.toast_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_radius(g.toast_label, 8, 0);
    lv_obj_set_style_pad_all(g.toast_label, 2, 0);
    lv_label_set_text(g.toast_label, " ");
    lv_obj_add_flag(g.toast_label, LV_OBJ_FLAG_HIDDEN);

    // --- 模态菜单浮层（盖住宠物区） ---
    g.menu_panel = make_panel(g.root, 72, 96);
    g.menu_title = make_panel_label(g.menu_panel, 8, "", COL_TEXT, LV_TEXT_ALIGN_CENTER);
    g.menu_item  = make_panel_label(g.menu_panel, 38, "", lv_color_hex(0x8B4513), LV_TEXT_ALIGN_CENTER);
    g.menu_hint  = make_panel_label(g.menu_panel, 68, "左右选 中键用 长按退", COL_TEXT, LV_TEXT_ALIGN_CENTER);

    // --- 特殊事件弹窗（需求 §5.4） ---
    g.ev_panel = make_panel(g.root, 64, 112);
    g.ev_title  = make_panel_label(g.ev_panel, 8, "", lv_color_hex(0xC00000), LV_TEXT_ALIGN_CENTER);
    g.ev_desc   = make_panel_label(g.ev_panel, 36, "", COL_TEXT, LV_TEXT_ALIGN_CENTER);
    g.ev_choices= make_panel_label(g.ev_panel, 64, "", lv_color_hex(0x005000), LV_TEXT_ALIGN_CENTER);
    g.ev_hint   = make_panel_label(g.ev_panel, 90, "限时选择…", COL_TEXT, LV_TEXT_ALIGN_CENTER);

    // --- 下图标行 ---
    lv_obj_t* bot_row = lv_obj_create(g.root);
    lv_obj_set_size(bot_row, 240, 40);
    lv_obj_set_pos(bot_row, 0, 196);
    lv_obj_set_style_bg_opa(bot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bot_row, 0, 0);
    lv_obj_set_style_pad_all(bot_row, 0, 0);
    lv_obj_clear_flag(bot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(bot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bot_row, LV_FLEX_ALIGN_SPACE_AROUND, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 4; i < 8; i++) {
        g.icon_objs[i] = make_icon(bot_row, kIcons[i].label, COL_ICON_BG);
    }
    return g.root;
}

}  // namespace

lv_obj_t* ui_main_create() {
    lv_obj_t* root = build_main();
    g.focus = 0;
    apply_focus(true);
    return root;
}

void ui_main_attach_key(bsp::KeyCallback cb) {
    g.user_cb = std::move(cb);
    bsp::buttons_set_callback(on_key);
}

void ui_main_attach_pet(PetCore* pet) {
    g.pet = pet;
    g.anim.attach(pet);
    if (pet) pet->subscribe(on_pet_event);
    // 渲染首帧 + 按当前灯光状态应用昼夜主题
    if (pet) {
        if (lvgl_port_lock(200)) {
            apply_day_night(pet->state().light_on);
            lvgl_port_unlock();
        }
    }
    if (g.pet_canvas && pet) {
        bool changed = false;
        const sprites::Sprite* s = g.anim.tick(0, &changed);
        if (s) {
            lv_color_t bg = pet->state().light_on
                ? COL_SKY : lv_color_hex(0x25315F);
            render_pet_sprite(g.pet_canvas, s, bg);
        }
    }
}

void ui_main_start_tick(uint8_t hours, uint8_t minutes) {
    // 时钟改由 wallclock 驱动，参数保留只为兼容旧签名
    (void)hours; (void)minutes;
    if (g_tick_timer) return;
    esp_timer_create_args_t cfg = {
        .callback = tick_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "ui_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &g_tick_timer);
    esp_timer_start_periodic(g_tick_timer, 100000ULL);  // 10Hz：刷新时钟(只每10次)+ sprite帧
}

bool ui_main_consume_want_game() {
    bool v = g.user_wants_game;
    g.user_wants_game = false;
    return v;
}

bool ui_main_pending_is_edu() { return g.pending_is_edu; }
uint8_t ui_main_pending_kind() { return g.pending_kind; }

bool ui_main_consume_want_status() {
    bool v = g.user_wants_status;
    g.user_wants_status = false;
    return v;
}

bool ui_main_consume_want_settings() {
    bool v = g.user_wants_settings;
    g.user_wants_settings = false;
    return v;
}

bool ui_main_consume_want_resurrect() {
    bool v = g.user_wants_resurrect;
    g.user_wants_resurrect = false;
    return v;
}

void ui_main_show_toast(const char* text, int duration_ms) {
    show_toast(text, duration_ms);
}

}  // namespace boxpet::ui
