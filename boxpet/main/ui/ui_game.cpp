// ui_game.cpp — 小游戏/课程场景（需求 §2.3 玩耍 + §2.5 教育）
// 模式（三按键适配）：
//   玩耍-丢球   ：左右猜（宠物看哪边）5 回合
//   玩耍-节奏   ：跟拍记忆（看序列→按左/右复现）5 步
//   教育-认字   ：找相同的字 5 题（左/中/右三选一）
//   教育-算术   ：加减法三选一 5 题
//   教育-音乐   ：跟拍记忆 5 步（同节奏）
//   教育-自由阅 ：自动阅读 8s，结束 +1 智力
// 布局（240x240）：
//   0-24   顶栏：标题 + 第N/5题
//   28-52  题面行：大字题目 / 提示
//   56-152 宠物精灵区
//   156-190 选项区（3 按钮 或 左右箭头）
//   216-240 底栏提示
#include "ui_game.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/audio.h"
#include "bsp/buttons.h"
#include "game/pet_def.h"
#include "game/pet_event.h"
#include "game/coins.h"
#include "anim.h"
#include "lvgl_sprite.h"
#include "coin_widget.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <cstdio>
#include <cstring>

static const char* TAG = "ui_game";

namespace boxpet::ui {

namespace {

using game::PetCore;

// ===== 模式 =====
enum class Mode : uint8_t {
    Ball,       // 玩耍-丢球（左右猜）
    Rhythm,     // 玩耍-节奏（跟拍）
    Word,       // 教育-认字（找相同字）
    Math,       // 教育-算术（三选一）
    Music,      // 教育-音乐（跟拍）
    Read,       // 教育-自由阅读（自动）
};

enum class Phase : uint8_t {
    Intro = 0,   // 待开始
    Show,        // 跟拍序列播放中
    Answer,      // 等玩家作答
    Feedback,    // 对/错反馈
    Done,        // 结算
};

constexpr int kQuestions = 5;        // 每局 5 题/步
// 认字字库（与 font_charset 同步生成）
static const char* kWordPool = "日月水火山木人口天地上中大花草鱼鸟";

struct GameUiState {
    lv_obj_t* root        = nullptr;
    lv_obj_t* title_label = nullptr;
    lv_obj_t* info_label  = nullptr;   // 第 N/5 题 胜 M
    lv_obj_t* q_label     = nullptr;   // 题面（大字）
    lv_obj_t* result_label= nullptr;   // 对!/错!/结算
    lv_obj_t* opt[3]      = {nullptr, nullptr, nullptr};  // 选项按钮
    lv_obj_t* pet_canvas  = nullptr;
    lv_obj_t* hint_label  = nullptr;   // 底栏提示

    Mode      mode        = Mode::Ball;
    bool      is_edu      = false;
    uint8_t   kind        = 0;
    Phase     phase       = Phase::Intro;
    int       q_index     = 0;      // 0..kQuestions-1
    int       correct     = 0;      // 答对数
    bool      began       = false;  // 已调用 play_begin/edu_begin
    bool      ended       = false;  // 已调用 play_end/edu_end
    // 题目数据
    int       target      = 0;      // Word: 目标字下标；Math: 正确值
    int       options[3]  = {0,0,0};// Math/Word: 3 个选项值
    int       math_a      = 0;      // Math: 操作数与运算
    int       math_b      = 0;
    bool      math_add    = true;
    bool      two_choice  = true;   // 左右二选一（vs 三选一）
    // 跟拍（Rhythm/Music）
    uint8_t   seq[kQuestions] = {0};
    int       show_step    = 0;
    int64_t   step_until_ms= 0;
    int       replay_step  = 0;
    // Read 自动计时
    int64_t   read_end_ms  = 0;
};
static GameUiState s;
static PetCore*       g_pet = nullptr;
static SpriteAnimator g_anim;
static esp_timer_handle_t g_tick_timer = nullptr;
static bool g_leave_game_flag = false;

static const lv_color_t COL_BG    = lv_color_hex(0xC0DCC0);
static const lv_color_t COL_BAR   = lv_color_hex(0x404040);
static const lv_color_t COL_TEXT  = lv_color_hex(0x202020);
static const lv_color_t COL_WHITE = lv_color_hex(0xFFFFFF);
static const lv_color_t COL_SKY   = lv_color_hex(0xBFE3F5);
static const lv_color_t COL_OK    = lv_color_hex(0x008000);
static const lv_color_t COL_BAD   = lv_color_hex(0xC00000);
static const lv_color_t COL_FOCUS = lv_color_hex(0xFF8C00);

static const char* mode_title(Mode m) {
    switch (m) {
        case Mode::Ball:     return "丢球";
        case Mode::Rhythm:   return "节奏";
        case Mode::Word:     return "认字";
        case Mode::Math:     return "算术";
        case Mode::Music:    return "音乐";
        case Mode::Read:     return "自由阅";
        default:             return "游戏";
    }
}

// ===== 出题 =====
static void make_question() {
    switch (s.mode) {
        case Mode::Ball:
            s.target = (int)(esp_random() % 2);   // 0=左 1=右
            s.two_choice = true;
            break;
        case Mode::Rhythm:
        case Mode::Music:
            s.two_choice = true;
            break;
        case Mode::Word: {
            s.two_choice = false;
            s.target = (int)(esp_random() % 16);
            // 3 个选项：1 正确 + 2 干扰（不同字）
            int pos = (int)(esp_random() % 3);
            for (int i = 0; i < 3; ++i) s.options[i] = -1;
            s.options[pos] = s.target;
            for (int i = 0; i < 3; ++i) {
                if (s.options[i] >= 0) continue;
                int d;
                do { d = (int)(esp_random() % 16); }
                while (d == s.target || d == s.options[0]
                       || d == s.options[1] || d == s.options[2]);
                s.options[i] = d;
            }
            break;
        }
        case Mode::Math: {
            s.two_choice = false;
            int a = 1 + (int)(esp_random() % 12);
            int b = 1 + (int)(esp_random() % 12);
            bool add = (esp_random() % 2) == 0;
            if (!add && b > a) { int t = a; a = b; b = t; }   // 保证非负
            s.math_a = a; s.math_b = b; s.math_add = add;
            s.target = add ? (a + b) : (a - b);
            // 3 个选项：正确 + 2 个相邻干扰
            int pos = (int)(esp_random() % 3);
            for (int i = 0; i < 3; ++i) s.options[i] = -1;
            s.options[pos] = s.target;
            int delta = 1 + (int)(esp_random() % 3);
            for (int i = 0; i < 3; ++i) {
                if (s.options[i] >= 0) continue;
                int d = s.target + (esp_random() % 2 ? delta : -delta);
                if (d < 0) d = s.target + delta;
                if (d == s.options[0] || d == s.options[1] || d == s.options[2])
                    d = d + delta + 1;
                s.options[i] = d;
            }
            break;
        }
        case Mode::Read:
            break;
    }
}

// ===== UI 刷新（需持锁）=====
static void update_info() {
    char buf[40];
    if (s.phase == Phase::Done) {
        snprintf(buf, sizeof(buf), "得分 %d/%d", s.correct, kQuestions);
    } else {
        snprintf(buf, sizeof(buf), "第%d/%d题 对%d", s.q_index + 1, kQuestions, s.correct);
    }
    lv_label_set_text(s.info_label, buf);
}

static void set_option_text(int i, const char* txt, bool visible) {
    if (!s.opt[i]) return;
    lv_label_set_text(lv_obj_get_child(s.opt[i], 0), txt);
    if (visible) lv_obj_clear_flag(s.opt[i], LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s.opt[i], LV_OBJ_FLAG_HIDDEN);
}

static void refresh_question() {
    if (!lvgl_port_lock(100)) return;
    update_info();
    switch (s.mode) {
        case Mode::Ball:
            lv_label_set_text(s.q_label, "球飞向哪边？");
            set_option_text(0, "<-- 左", true);
            set_option_text(1, "", false);
            set_option_text(2, "右 -->", true);
            lv_label_set_text(s.result_label, " ");
            break;
        case Mode::Word: {
            // kWordPool 是 UTF-8：每个汉字 3 字节。s.target 是"字序号"，
            // 必须拷 3 字节——之前只拷 1 字节（b[0] = kWordPool[s.target]）
            // 拼出非法 UTF-8 → LVGL 渲染成方框（与字体无关）。
            char b[8] = {0};
            int ti = s.target;
            if (ti >= 0 && ti < 16) memcpy(b, &kWordPool[ti * 3], 3);
            else                   snprintf(b, sizeof(b), "?");
            char buf[24];
            snprintf(buf, sizeof(buf), "找「%s」", b);
            lv_label_set_text(s.q_label, buf);
            for (int i = 0; i < 3; ++i) {
                char ob[8] = {0};
                int oi = s.options[i];
                if (oi >= 0 && oi < 16) memcpy(ob, &kWordPool[oi * 3], 3);
                else                    snprintf(ob, sizeof(ob), "?");
                set_option_text(i, ob, true);
            }
            lv_label_set_text(s.result_label, " ");
            break;
        }
        case Mode::Math: {
            char buf[24];
            snprintf(buf, sizeof(buf), "%d %s %d = ?", s.math_a,
                     s.math_add ? "+" : "-", s.math_b);
            lv_label_set_text(s.q_label, buf);
            char ob[3][8];
            for (int i = 0; i < 3; ++i) {
                snprintf(ob[i], sizeof(ob[i]), "%d", s.options[i]);
                set_option_text(i, ob[i], true);
            }
            lv_label_set_text(s.result_label, " ");
            break;
        }
        case Mode::Rhythm:
        case Mode::Music:
            set_option_text(0, "<-- 左", true);
            set_option_text(1, "", false);
            set_option_text(2, "右 -->", true);
            lv_label_set_text(s.result_label, " ");
            break;
        default:
            break;
    }
    lvgl_port_unlock();
}

// ===== 流程控制 =====
static void start_game();

static void finish_game() {
    if (s.ended) return;
    s.ended = true;
    s.phase = Phase::Done;
    bool win = (s.mode == Mode::Read) || (s.correct * 2 > kQuestions);
    if (g_pet && s.began) {
        if (s.is_edu) g_pet->edu_end((game::EduKind)s.kind, s.correct);
        else          g_pet->play_end((game::PlayKind)s.kind, win);
    }
    bsp::audio_play(win ? bsp::Sound::Win : bsp::Sound::Lose);
    if (!lvgl_port_lock(100)) return;
    update_info();
    char b[32];
    snprintf(b, sizeof(b), "%d对%d错", s.correct, kQuestions - s.correct);
    lv_label_set_text(s.result_label, b);
    lv_obj_set_style_text_color(s.result_label, win ? COL_OK : COL_BAD, 0);
    lv_label_set_text(s.q_label, s.correct == kQuestions ? "全部答对！" :
                                (win ? "表现不错！" : "下次努力"));
    lv_label_set_text(s.hint_label, "中键返回 长按退出");
    set_option_text(0, "", false);
    set_option_text(1, "", false);
    set_option_text(2, "", false);
    lvgl_port_unlock();

    // 金币结算（教育 vs 玩耍用不同公式）
    int32_t reward = 0;
    if (s.is_edu) {
        reward = game::calc_edu_reward(s.correct, kQuestions);
    } else {
        // 玩耍：剩余精力比例（0~1）作为系数
        float energy_remain = g_pet ? (g_pet->state().energy / 100.0f) : 1.0f;
        reward = game::calc_play_reward(win, energy_remain);
    }
    if (reward > 0) {
        game::coins_add(reward);
        bsp::audio_play(bsp::Sound::Correct);  // 短促反馈音
        coin_widget_float_text((int)reward);   // 飘字（在顶层屏幕）
    }
}

static void next_question() {
    if (s.q_index + 1 >= kQuestions) { finish_game(); return; }
    s.q_index++;
    make_question();
    s.phase = (s.mode == Mode::Rhythm || s.mode == Mode::Music)
              ? Phase::Show : Phase::Answer;
    if (s.phase == Phase::Show) {
        // 生成新序列
        for (int i = 0; i < kQuestions; ++i) s.seq[i] = (uint8_t)(esp_random() % 2);
        s.show_step = 0;
        s.replay_step = 0;
        s.step_until_ms = esp_timer_get_time() / 1000 + 600;
    }
    refresh_question();
}

static void start_game() {
    if (!g_pet) return;
    // 资格复查（ui_main 已查过，双保险）
    int why = 0;
    if (s.is_edu) {
        if (!g_pet->can_learn((game::EduKind)s.kind, &why)) {
            if (!lvgl_port_lock(100)) return;
            lv_label_set_text(s.result_label, "现在不能学");
            lv_label_set_text(s.hint_label, "中键返回 长按退出");
            lvgl_port_unlock();
            s.phase = Phase::Done;
            s.ended = true;   // 不结算
            return;
        }
        g_pet->edu_begin((game::EduKind)s.kind);
    } else {
        if (!g_pet->can_play((game::PlayKind)s.kind, &why)) {
            if (!lvgl_port_lock(100)) return;
            lv_label_set_text(s.result_label, "现在不能玩");
            lv_label_set_text(s.hint_label, "中键返回 长按退出");
            lvgl_port_unlock();
            s.phase = Phase::Done;
            s.ended = true;
            return;
        }
        g_pet->play_begin((game::PlayKind)s.kind);
    }
    s.began = true;
    s.q_index = 0;
    s.correct = 0;

    if (s.mode == Mode::Read) {
        s.phase = Phase::Show;   // 自动阅读（Show 复用为自动阶段）
        s.read_end_ms = esp_timer_get_time() / 1000 + 8000;
        if (lvgl_port_lock(100)) {
            lv_label_set_text(s.q_label, "认真读书中…");
            lv_label_set_text(s.result_label, " ");
            update_info();
            lvgl_port_unlock();
        }
        return;
    }
    s.phase = Phase::Answer;
    make_question();
    if (s.mode == Mode::Rhythm || s.mode == Mode::Music) {
        s.phase = Phase::Show;
        for (int i = 0; i < kQuestions; ++i) s.seq[i] = (uint8_t)(esp_random() % 2);
        s.show_step = 0;
        s.replay_step = 0;
        s.step_until_ms = esp_timer_get_time() / 1000 + 600;
    }
    refresh_question();
}

// 作答：choice 0=左 1=中 2=右
static void submit_answer(int choice) {
    if (s.phase != Phase::Answer) return;
    bool ok = false;
    switch (s.mode) {
        case Mode::Ball:
            ok = (choice == (s.target == 0 ? 0 : 2));   // 左=0 右=2
            break;
        case Mode::Word:
        case Mode::Math: {
            // options[choice] 是否等于 target
            ok = (s.options[choice] == s.target);
            break;
        }
        case Mode::Rhythm:
        case Mode::Music: {
            // 跟拍：choice 0=左 2=右；对照 seq[replay_step]
            int val = (choice == 0) ? 0 : 1;
            ok = (val == s.seq[s.replay_step]);
            ++s.replay_step;
            if (ok && s.replay_step >= kQuestions) {
                // 全部复现成功 → 本题（整局）完
                s.correct = kQuestions;
                s.q_index = kQuestions - 1;
                finish_game();
                return;
            }
            if (!ok) {
                // 中途错 → 结束（correct=已对步数）
                s.correct = s.replay_step - 1;
                s.q_index = kQuestions - 1;
                finish_game();
                return;
            }
            // 继续下一步
            s.phase = Phase::Feedback;
            s.step_until_ms = esp_timer_get_time() / 1000 + 300;
            if (lvgl_port_lock(100)) {
                lv_label_set_text(s.result_label, ok ? "对!" : "错!");
                lv_obj_set_style_text_color(s.result_label, ok ? COL_OK : COL_BAD, 0);
                lvgl_port_unlock();
            }
            bsp::audio_play(ok ? bsp::Sound::Correct : bsp::Sound::Wrong);
            return;
        }
        default: break;
    }
    if (ok) s.correct++;
    s.phase = Phase::Feedback;
    s.step_until_ms = esp_timer_get_time() / 1000 + 800;
    if (lvgl_port_lock(100)) {
        update_info();
        lv_label_set_text(s.result_label, ok ? "对!" : "错!");
        lv_obj_set_style_text_color(s.result_label, ok ? COL_OK : COL_BAD, 0);
        lvgl_port_unlock();
    }
    bsp::audio_play(ok ? bsp::Sound::Correct : bsp::Sound::Wrong);
}

static void on_key_in_game(bsp::KeyId id, bsp::KeyEvent evt) {
    if (evt == bsp::KeyEvent::ShortPress) {
        if (!lvgl_port_lock(100)) return;
        int choice = (id == bsp::KeyId::Left) ? 0
                   : (id == bsp::KeyId::Right) ? 2 : 1;
        switch (s.phase) {
            case Phase::Intro:
                if (id == bsp::KeyId::Mid) {
                    lvgl_port_unlock();
                    start_game();
                    bsp::audio_play(bsp::Sound::Beep);
                    return;
                }
                break;
            case Phase::Answer:
                // 二选一：只认左/右；三选一：左/中/右
                if (s.two_choice && choice == 1) break;
                if (!s.two_choice || choice != 1) {
                    lvgl_port_unlock();
                    submit_answer(choice);
                    return;
                }
                break;
            case Phase::Done:
                if (id == bsp::KeyId::Mid) g_leave_game_flag = true;
                break;
            default:
                break;
        }
        lvgl_port_unlock();
    } else if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        // 提前退出：已开始未结束 → 按当前成绩结算
        if (s.began && !s.ended) finish_game();
        g_leave_game_flag = true;
    }
}

static lv_obj_t* make_option(lv_obj_t* parent, int x, int w) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, 34);
    lv_obj_set_pos(o, x, 156);
    lv_obj_set_style_bg_color(o, COL_TEXT, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_30, 0);
    lv_obj_set_style_border_color(o, COL_TEXT, 0);
    lv_obj_set_style_border_width(o, 2, 0);
    lv_obj_set_style_radius(o, 8, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* lbl = lv_label_create(o);
    // 关键顺序：先 set_font，再 set_text——LVGL v9 在 set_text 时按当前 font
    // 查 glyph_dsc；若先 set_text 后 set_font，缓存可能错过有效字符。
    lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl, ui_font_16, 0);
    lv_label_set_text(lbl, "");
    lv_obj_center(lbl);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static lv_obj_t* build_game_ui() {
    lv_obj_t* root = lv_obj_create(nullptr);
    lv_obj_set_size(root, 240, 240);
    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // --- 顶栏 ---
    lv_obj_t* top = lv_obj_create(root);
    lv_obj_set_size(top, 240, 24);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, COL_BAR, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_set_style_pad_all(top, 0, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);

    s.title_label = lv_label_create(top);
    lv_label_set_text(s.title_label, mode_title(s.mode));
    lv_obj_set_style_text_color(s.title_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s.title_label, ui_font_16, 0);
    lv_obj_align(s.title_label, LV_ALIGN_LEFT_MID, 8, 0);

    s.info_label = lv_label_create(top);
    lv_label_set_text(s.info_label, "");
    lv_obj_set_style_text_color(s.info_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s.info_label, ui_font_16, 0);
    lv_obj_align(s.info_label, LV_ALIGN_RIGHT_MID, -8, 0);

    // --- 题面行 ---
    s.q_label = lv_label_create(root);
    lv_obj_set_pos(s.q_label, 0, 30);
    lv_obj_set_size(s.q_label, 240, 24);
    lv_obj_set_style_text_color(s.q_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s.q_label, ui_font_16, 0);
    lv_obj_set_style_text_align(s.q_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s.q_label, "中键开始");

    // --- 结果行（宠物区上方）---
    s.result_label = lv_label_create(root);
    lv_obj_set_pos(s.result_label, 0, 56);
    lv_obj_set_size(s.result_label, 240, 22);
    lv_obj_set_style_text_color(s.result_label, COL_TEXT, 0);
    lv_obj_set_style_text_font(s.result_label, ui_font_16, 0);
    lv_obj_set_style_text_align(s.result_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s.result_label, " ");

    // --- 宠物精灵 ---
    s.pet_canvas = create_pet_canvas_at(root, (240 - 96) / 2, 82);

    // --- 选项区（3 个按钮：二选一时隐藏中间）---
    s.opt[0] = make_option(root, 8, 70);
    s.opt[1] = make_option(root, 85, 70);
    s.opt[2] = make_option(root, 162, 70);

    // --- 底栏 ---
    lv_obj_t* bot = lv_obj_create(root);
    lv_obj_set_size(bot, 240, 24);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_style_bg_color(bot, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    s.hint_label = lv_label_create(bot);
    lv_label_set_text(s.hint_label, "中键开始 长按退出");
    lv_obj_set_style_text_color(s.hint_label, COL_WHITE, 0);
    lv_obj_set_style_text_font(s.hint_label, ui_font_16, 0);
    lv_obj_center(s.hint_label);
    return root;
}

// tick：Feedback 超时→下一题；Show 播放序列；Read 自动结束（需持锁部分内部处理）
static void game_logic_tick(int64_t now_ms) {
    switch (s.phase) {
        case Phase::Show:
            if (s.mode == Mode::Read) {
                if (now_ms >= s.read_end_ms) {
                    s.correct = 1;   // 阅读完成 +1 智力（edu_end correct=1）
                    finish_game();
                }
                return;
            }
            // 跟拍序列播放
            if (now_ms >= s.step_until_ms) {
                s.show_step++;
                s.step_until_ms = now_ms + 600;
                bsp::audio_play(bsp::Sound::Tick);   // 每步节拍音
                if (s.show_step > kQuestions) {
                    s.phase = Phase::Answer;
                    s.replay_step = 0;
                    if (lvgl_port_lock(100)) {
                        lv_label_set_text(s.q_label, "轮到你！按左/右");
                        lvgl_port_unlock();
                    }
                }
            }
            break;
        case Phase::Feedback:
            if (now_ms >= s.step_until_ms) {
                // 跟拍中间反馈 → 继续 Answer；普通题 → 下一题
                if ((s.mode == Mode::Rhythm || s.mode == Mode::Music)
                    && s.replay_step < kQuestions) {
                    s.phase = Phase::Answer;
                } else {
                    next_question();
                }
            }
            break;
        default:
            break;
    }
}

static void tick_timer_cb(void* /*arg*/) {
    if (!s.root) return;
    int64_t now_ms = esp_timer_get_time() / 1000;
    game_logic_tick(now_ms);
    if (!lvgl_port_lock(50)) return;
    // 跟拍播放：高亮当前步的方向按钮
    if (s.phase == Phase::Show && (s.mode == Mode::Rhythm || s.mode == Mode::Music)) {
        int step = s.show_step;
        bool flash = ((now_ms / 300) % 2) == 0;
        int lit = (step < kQuestions) ? s.seq[step] : -1;
        lv_obj_set_style_bg_opa(s.opt[0], (lit == 0 && flash) ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_bg_opa(s.opt[2], (lit == 1 && flash) ? LV_OPA_COVER : LV_OPA_30, 0);
        lv_obj_set_style_border_color(s.opt[0], (lit == 0) ? COL_FOCUS : COL_TEXT, 0);
        lv_obj_set_style_border_color(s.opt[2], (lit == 1) ? COL_FOCUS : COL_TEXT, 0);
        if (s.opt[1]) lv_obj_set_style_bg_opa(s.opt[1], LV_OPA_30, 0);
        char b[24];
        snprintf(b, sizeof(b), "看仔细 %d/%d", step + 1, kQuestions);
        lv_label_set_text(s.q_label, b);
    } else {
        lv_obj_set_style_bg_opa(s.opt[0], LV_OPA_30, 0);
        lv_obj_set_style_bg_opa(s.opt[2], LV_OPA_30, 0);
    }
    // 精灵帧：Feedback 期间显示 happy/scold，其余用 animator idle 帧
    bool changed = false;
    const sprites::Sprite* frame = g_anim.tick(now_ms, &changed);
    if (s.phase == Phase::Feedback) {
        const char* txt = lv_label_get_text(s.result_label);
        bool ok = (txt && (unsigned char)txt[0] == 0xE5u);   // "对" UTF-8 首字节
        // 按 stage 选参数化帧（需求3：失败 scold 帧颜色与正常状态一致）
        frame = find_stage_sprite(ok ? "happy" : "scold", g_pet->state());
        changed = true;
    }
    if (frame && changed) render_pet_sprite(s.pet_canvas, frame, COL_SKY);
    lvgl_port_unlock();
}

}  // namespace

void ui_game_configure(bool is_edu, uint8_t kind) {
    s.is_edu = is_edu;
    s.kind = kind;
    if (!is_edu) {
        switch ((game::PlayKind)kind) {
            case game::PlayKind::Rhythm: s.mode = Mode::Rhythm; break;
            default:                     s.mode = Mode::Ball;  break;   // Ball/Free
        }
    } else {
        switch ((game::EduKind)kind) {
            case game::EduKind::Math:  s.mode = Mode::Math;  break;
            case game::EduKind::Music: s.mode = Mode::Music; break;
            case game::EduKind::Read:  s.mode = Mode::Read;  break;
            default:                   s.mode = Mode::Word;  break;
        }
    }
}

lv_obj_t* ui_game_create() {
    s.root = build_game_ui();
    s.phase = Phase::Intro;
    s.q_index = 0;
    s.correct = 0;
    s.began = false;
    s.ended = false;
    g_anim.attach(g_pet);
    // 首帧
    if (s.pet_canvas) {
        const sprites::Sprite* f = find_sprite_by_name("child");
        if (!f) f = g_anim.tick(0, nullptr);
        if (f) render_pet_sprite(s.pet_canvas, f, COL_SKY);
    }
    bsp::buttons_set_callback(on_key_in_game);
    // 10Hz tick
    esp_timer_create_args_t cfg = {
        .callback = tick_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "game_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &g_tick_timer);
    esp_timer_start_periodic(g_tick_timer, 100000ULL);
    return s.root;
}

void ui_game_set_pet(::boxpet::game::PetCore* pet) {
    g_pet = pet;
    g_anim.attach(pet);
}

void ui_game_start() {
    // 等待用户按中键（Intro）
}

void ui_game_close() {
    // 退出前兜底结算
    if (s.began && !s.ended) finish_game();
    if (g_tick_timer) {
        esp_timer_stop(g_tick_timer);
        esp_timer_delete(g_tick_timer);
        g_tick_timer = nullptr;
    }
    // 删除屏幕对象（修复泄漏，见 ui_status_close 注释）
    if (s.root && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.root);
        lvgl_port_unlock();
    }
    s.root = nullptr;
}

bool ui_game_wants_to_leave() { return g_leave_game_flag; }
void ui_game_clear_leave_flag() { g_leave_game_flag = false; }

}  // namespace boxpet::ui
