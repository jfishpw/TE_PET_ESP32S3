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
#include "esp_heap_caps.h"
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
    Counter,    // 教育-计数器（亿/万/个级拨珠位值教学）
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

// ===== 音乐课：下落式金币打点（v5 重做）=====
// 金币从屏幕上方左右两条轨道落下，经过底部判定线时按对应方向键：
// 命中 +1 分（高音反馈），漏掉不得分；全程有节拍伴奏；结算按分数奖励魔力+金币。
// 一局约 30 秒（60 音符 × 500ms 节拍）。
constexpr int     kMusNotes    = 60;    // 一局音符数（×500ms ≈ 30 秒）
constexpr int     kMusBeatMs   = 500;   // 节拍间隔（伴奏/金币节奏）
constexpr int     kMusFallMs   = 1150;  // 金币从顶部落到判定线的时间
constexpr int     kMusTopY     = 40;    // 轨道顶部（金币出现点，尽量高）
constexpr int     kMusHitY     = 176;   // 判定线 y
constexpr int     kMusCoinSize = 18;    // 金币尺寸（略大更醒目）
constexpr int64_t kMusHitWindow= 175;   // 命中判定窗口（±ms）
constexpr int     kMusCoinPool = 8;     // 金币对象池（同屏最多约 4~5 枚）
// 两条轨道金币左边缘 x（关于屏幕中心 120 对称：中心 44 与 196）
constexpr int     kMusLaneX[2] = {35, 187};

struct MNote { int lane; int64_t hit_ms; uint8_t state; };  // state 0=待判 1=命中 2=漏

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
    // Counter（拨珠计数器）：value=0~999999999（9 位到亿），digit=选中位
    int64_t   counter_value = 0;
    int       counter_digit = 0;   // 0=个 1=十 2=百 3=千 4=万 5=十万 6=百万 7=千万 8=亿
    // Counter 专用控件：竖条彩色算珠画布 + 每列位数字 + 右上迷你宠物
    lv_obj_t* ctr_canvas   = nullptr;          // 240x104 算珠画布
    lv_color_t* ctr_cbuf   = nullptr;          // PSRAM 缓冲（delete 回调释放）
    lv_obj_t* ctr_digits[9] = {nullptr};       // 每列当前位数字（0-9）
    lv_obj_t* ctr_names[9]  = {nullptr};       // 每列下方竖排位名（个/十/…/亿）
    lv_obj_t* ctr_pet      = nullptr;          // 右上角 48x48 迷你宠物
    lv_color_t* ctr_pet_cbuf = nullptr;
    bool        ctr_pet_drawn = false;          // 迷你宠物已渲染（仅首次）
    // ===== 音乐课（下落金币打点）=====
    lv_obj_t* mus_coin[kMusCoinPool] = {nullptr};  // 金币对象池
    int8_t    mus_slot_owner[kMusCoinPool] = {-1,-1,-1,-1,-1,-1,-1,-1};  // 槽位归属音符
    lv_obj_t* mus_pad[2]  = {nullptr};         // 左右判定垫（命中闪光）
    lv_obj_t* mus_line[2] = {nullptr};         // 左右判定线
    MNote     mus_notes[kMusNotes];            // 本局音符
    int64_t   mus_next_beat = 0;               // 下个伴奏节拍时刻
    int       mus_beat_idx  = 0;               // 伴奏旋律下标
    int       mus_score     = 0;               // 命中数
    int       mus_done      = 0;               // 已判定（命中+漏）
    int64_t   mus_pad_until[2] = {0, 0};       // 判定垫闪光截止
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
        case Mode::Counter:  return "计数器";
        default:             return "游戏";
    }
}

// ===== 计数器（亿/万/个级拨珠位值教学）=====
static const int64_t kPow10[9] = {1, 10, 100, 1000, 10000, 100000, 1000000, 10000000, 100000000};
static const char* kDigitNames[9] = {"个位", "十位", "百位", "千位",
                                      "万位", "十万位", "百万位", "千万位", "亿位"};
static constexpr int64_t kCounterMax = 999999999;   // 9 位（到亿位）
// 算珠画布几何：左→右 = 亿→个（列 0 在左），个位列在最右。
// kCtrColX=16 使 9 列关于屏幕中心(120)对称：中心列(第5列)恰在 x=120，
// 左右边距均为 12px，列间距视觉均匀（原 8 导致整体偏左 8px，观感错乱）。
static constexpr int kCtrCanvasW = 240;
static constexpr int kCtrCanvasH = 100;     // 竖条+珠子区（下方留给竖排位名）
static constexpr int kCtrColX    = 16;      // 首列中心 x（居中）
static constexpr int kCtrColStep = 26;      // 列间距
static constexpr int kCtrBeadR   = 3;       // 珠子半径（珠间距更清爽均匀）
// 每列下方竖排位名（索引=digit：0 个 → 8 亿）；两字位名用 \n 竖排
static const char* kDigitVertNames[9] = {"个", "十", "百", "千", "万",
                                          "十\n万", "百\n万", "千\n万", "亿"};
// 算珠配色：未选中两色交替，选中位整列橙色
static lv_color_t gBeadA = lv_color_hex(0x3AC0FF);   // 青
static lv_color_t gBeadB = lv_color_hex(0x27AE60);   // 绿
static lv_color_t gBeadSel = lv_color_hex(0xFF8C00); // 橙

// 实心圆（写画布像素，调用方已持锁）
static void bead_circle(lv_obj_t* cv, int cx, int cy, int r, lv_color_t c) {
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx)
            if (dx * dx + dy * dy <= r * r)
                lv_canvas_set_px(cv, cx + dx, cy + dy, c, LV_OPA_COVER);
}

// 画竖条彩色算珠：每列一根竖条，底部向上亮起 digit 颗珠子；选中位橙色
static void draw_counter_beads_locked() {
    if (!s.ctr_canvas) return;
    lv_canvas_fill_bg(s.ctr_canvas, lv_color_hex(0x16212E), LV_OPA_COVER);   // 深色衬底
    for (int d = 0; d < 9; ++d) {
        const int col = 8 - d;                       // d=0(个位)→最右列
        const int cx  = kCtrColX + col * kCtrColStep;
        const int val = (int)((s.counter_value / kPow10[d]) % 10);
        const bool sel = (d == s.counter_digit);
        // 竖条（珠子区竖向浅色引导线：恰好覆盖珠子上下边界，不越界出线头）
        const int y_top = kCtrCanvasH - 6 - 9 * 10;
        for (int y = y_top; y < kCtrCanvasH - 6; ++y)
            lv_canvas_set_px(s.ctr_canvas, cx, y, lv_color_hex(0x5A7A92), LV_OPA_COVER);
        // 珠子：底部向上亮起 val 颗（格中心 10px，珠距均匀）
        for (int k = 0; k < val; ++k) {
            const int cy = (kCtrCanvasH - 6) - 5 - k * 10;
            lv_color_t c = sel ? gBeadSel : ((k & 1) ? gBeadA : gBeadB);
            bead_circle(s.ctr_canvas, cx, cy, kCtrBeadR, c);
        }
    }
}

// 每列顶部位数字（0-9），选中位橙色。
// 数字标签从左到右与算珠列一致（亿→个）：digit d 显示在标签 [8-d]
static void update_counter_digits_locked() {
    for (int d = 0; d < 9; ++d) {
        lv_obj_t* lbl = s.ctr_digits[8 - d];
        if (!lbl) continue;
        const int v = (int)((s.counter_value / kPow10[d]) % 10);
        char t[2] = {(char)('0' + v), 0};
        lv_label_set_text(lbl, t);
        lv_obj_set_style_text_color(lbl,
            (d == s.counter_digit) ? gBeadSel : lv_color_hex(0x80A0B8), 0);
    }
}

// 右上角迷你宠物：48x48 精灵按 1/48 采样缩到 kMiniW×kMiniH（1:1 逐像素抽点）
static constexpr int kMiniW = 28;
static constexpr int kMiniH = 28;
static void render_mini_pet_locked(const sprites::Sprite* f) {
    if (!s.ctr_pet || !f) return;
    lv_canvas_fill_bg(s.ctr_pet, lv_color_hex(0x1A2333), LV_OPA_COVER);
    for (int y = 0; y < kMiniH; ++y) {
        for (int x = 0; x < kMiniW; ++x) {
            int sx = x * 48 / kMiniW, sy = y * 48 / kMiniH;
            uint8_t byte = f->bitmap[(sy * 48 + sx) / 2];
            uint8_t idx = (sx & 1) ? (byte & 0x0F) : (byte >> 4);
            if (idx == 0) continue;   // 透明
            lv_canvas_set_px(s.ctr_pet, x, y,
                             lv_color_hex(sprites::kPalette[idx]), LV_OPA_COVER);
        }
    }
    lv_obj_invalidate(s.ctr_pet);
}

// 刷新计数器显示（调用方需已持 LVGL 锁）：
//  result_label: 当前选中位 + 操作提示
//  info_label  : 三级名称提示；算珠画布 + 每列位数字同步更新
static void refresh_counter_locked() {
    char r[40];
    snprintf(r, sizeof(r), "选中: %s  中键+1", kDigitNames[s.counter_digit]);
    lv_label_set_text(s.result_label, r);
    lv_obj_set_style_text_color(s.result_label, COL_TEXT, 0);
    lv_label_set_text(s.info_label, "亿级 万级 个级");
    lv_label_set_text(s.hint_label, "左右选位 中键+1 长按结束");
    update_counter_digits_locked();
    draw_counter_beads_locked();
    if (!s.ctr_pet_drawn) {
        s.ctr_pet_drawn = true;
        render_mini_pet_locked(g_pet ? idle_frame_for(g_pet->state()) : nullptr);
    }
}

// ===== 出题 =====
static void make_question() {
    switch (s.mode) {
        case Mode::Ball:
            s.target = (int)(esp_random() % 2);   // 0=左 1=右
            s.two_choice = true;
            break;
        case Mode::Counter:
            s.two_choice = true;   // 计数器不出题（防御分支，实际不可达）
            break;
        case Mode::Rhythm:
            s.two_choice = false;   // 三键三轨：左/中/右（v5 优化）
            break;
        case Mode::Music:
            s.two_choice = true;    // 音乐课保持双键（左/右）
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
    } else if (s.mode == Mode::Rhythm || s.mode == Mode::Music) {
        // 跟拍局：显示复现进度（原“第1/5题”对跟拍无意义）
        snprintf(buf, sizeof(buf), "跟拍 %d/%d 步", s.replay_step, kQuestions);
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
        case Mode::Counter:
            break;   // 计数器显示由 refresh_counter_locked 负责（防御分支）
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
            // 三键三轨：左/中/右
            set_option_text(0, "<-- 左", true);
            set_option_text(1, "中", true);
            set_option_text(2, "右 -->", true);
            lv_label_set_text(s.result_label, " ");
            break;
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
// 音乐课（定义在下方流程区）：前置声明供 start_game / 按键处理调用
static void music_ui_visible(bool show);
static void music_begin_locked();
static void music_hit(int lane);

static void finish_game() {
    if (s.ended) return;
    s.ended = true;
    s.phase = Phase::Done;

    // ===== 音乐课结算：按分数奖励魔力属性 + 金币 =====
    if (s.mode == Mode::Music) {
        int score = s.mus_score, total = kMusNotes;
        bool win = (score * 2 > total);
        float magic = (float)score * 0.5f;                 // 每命中 +0.5 魔力
        if (g_pet && s.began) {
            int norm = (total > 0) ? (score * kQuestions) / total : 0;  // 归一到 5 分制
            g_pet->edu_end(game::EduKind::Music, norm);    // 智力/心情/技能 + 基础魔力
            g_pet->add_growth(0, magic, 0);                // 按分数追加魔力
        }
        bsp::audio_play(win ? bsp::Sound::Win : bsp::Sound::Lose);
        int32_t reward = game::calc_edu_reward(score, total);
        if (!lvgl_port_lock(100)) return;
        char b[40];
        snprintf(b, sizeof(b), "命中 %d/%d", score, total);
        lv_label_set_text(s.result_label, b);
        lv_obj_set_style_text_color(s.result_label, win ? COL_OK : COL_BAD, 0);
        snprintf(b, sizeof(b), "魔力 +%.1f  金币 +%ld", magic, (long)reward);
        lv_label_set_text(s.info_label, b);
        if (s.hint_label) lv_label_set_text(s.hint_label, "中键返回 长按退出");
        lvgl_port_unlock();
        if (reward > 0) {
            game::coins_add(reward);
            coin_widget_float_text((int)reward);
        }
        return;
    }

    // 计数器完成即赢（无对错概念，结算 +1 智力）
    bool win = (s.mode == Mode::Read) || (s.mode == Mode::Counter)
            || (s.correct * 2 > kQuestions);
    if (g_pet && s.began) {
        if (s.is_edu) g_pet->edu_end((game::EduKind)s.kind,
                                     (s.mode == Mode::Counter) ? 1 : s.correct);
        else          g_pet->play_end((game::PlayKind)s.kind, win);
    }
    bsp::audio_play(win ? bsp::Sound::Win : bsp::Sound::Lose);
    if (!lvgl_port_lock(100)) return;
    update_info();
    char b[32];
    if (s.mode == Mode::Counter) {
        // 计数器结算：显示最终数值，不涉及"对/错"
        snprintf(b, sizeof(b), "结果 %lld", (long long)s.counter_value);
        lv_label_set_text(s.result_label, b);
        lv_obj_set_style_text_color(s.result_label, COL_OK, 0);
        lv_label_set_text(s.q_label, "计数完成！");
        lv_label_set_text(s.info_label, "智力 +1");
    } else {
        snprintf(b, sizeof(b), "%d对%d错", s.correct, kQuestions - s.correct);
        lv_label_set_text(s.result_label, b);
        lv_obj_set_style_text_color(s.result_label, win ? COL_OK : COL_BAD, 0);
        lv_label_set_text(s.q_label, s.correct == kQuestions ? "全部答对！" :
                                    (win ? "表现不错！" : "下次努力"));
    }
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
    if (s.mode == Mode::Counter) {
        // 计数器：无题无轮，直接进入拨珠状态（左右换位、中键+1）
        s.counter_value = 0;
        s.counter_digit = 0;
        s.phase = Phase::Answer;
        s.correct = 0;
        if (lvgl_port_lock(100)) {
            refresh_counter_locked();
            lvgl_port_unlock();
        }
        return;
    }
    s.phase = Phase::Answer;
    make_question();
    if (s.mode == Mode::Music) {
        // 音乐课：下落金币打点（伴奏+判定线），进入演奏阶段
        s.phase = Phase::Show;
        if (lvgl_port_lock(100)) {
            music_ui_visible(true);
            music_begin_locked();
            lvgl_port_unlock();
        }
        return;
    }
    if (s.mode == Mode::Rhythm) {
        s.phase = Phase::Show;
        // 节奏：三轨（0/1/2）跟拍记忆
        for (int i = 0; i < kQuestions; ++i)
            s.seq[i] = (uint8_t)(esp_random() % 3);
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
        case Mode::Counter:
            break;   // 计数器无对错判定（按键已在上层拦截；防御分支）
        case Mode::Word:
        case Mode::Math: {
            // options[choice] 是否等于 target
            ok = (s.options[choice] == s.target);
            break;
        }
        case Mode::Rhythm: {
            // 三轨跟拍：左=0 中=1 右=2，对照 seq[replay_step]
            ok = (choice == s.seq[s.replay_step]);
            ++s.replay_step;
            if (ok && s.replay_step >= kQuestions) {
                // 全部复现成功 → 整局完
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
            // 单步正确 → 短反馈（300ms）后继续下一步
            s.phase = Phase::Feedback;
            s.step_until_ms = esp_timer_get_time() / 1000 + 300;
            if (lvgl_port_lock(100)) {
                lv_label_set_text(s.result_label, "对!");
                lv_obj_set_style_text_color(s.result_label, COL_OK, 0);
                update_info();
                lvgl_port_unlock();
            }
            bsp::audio_play(bsp::Sound::Correct);
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
            case Phase::Show:
                // 音乐课：演奏阶段左右键打点（中键不参与）
                if (s.mode == Mode::Music) {
                    int lane = (id == bsp::KeyId::Left) ? 0
                             : (id == bsp::KeyId::Right) ? 1 : -1;
                    lvgl_port_unlock();
                    if (lane >= 0) music_hit(lane);
                    return;
                }
                break;
            case Phase::Answer:
                // 计数器拨珠：左右键换选中位，中键给当前位 +1（满 9 位回绕）。
                // 方向与画面一致：左键=向左移（个→…→亿），右键=向右移（亿→…→个）。
                if (s.mode == Mode::Counter) {
                    if (id == bsp::KeyId::Left) {
                        s.counter_digit = (s.counter_digit + 1) % 9;
                        refresh_counter_locked();
                        bsp::audio_play(bsp::Sound::Tick);
                    } else if (id == bsp::KeyId::Right) {
                        s.counter_digit = (s.counter_digit + 8) % 9;
                        refresh_counter_locked();
                        bsp::audio_play(bsp::Sound::Tick);
                    } else if (id == bsp::KeyId::Mid) {
                        s.counter_value += kPow10[s.counter_digit];
                        if (s.counter_value > kCounterMax) s.counter_value = 0;
                        refresh_counter_locked();
                        bsp::audio_play(bsp::Sound::Correct);   // 拨珠反馈音
                    }
                    break;   // 保持计数会话（由底部统一解锁）
                }
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

// canvas 缓冲随对象销毁释放（与 lvgl_sprite 相同机制，防反复进出泄漏）
// 注意缓冲必须从 PSRAM 分配：LVGL 内部堆仅 64KB，游戏页 96x96 宠物已占
// 18KB，计数器画布 48KB 放内部堆必分配失败 → 整个计数器界面空白。
static void game_canvas_free_cb(lv_event_t* e) {
    void* buf = lv_event_get_user_data(e);
    if (buf) heap_caps_free(buf);
}

// ===== 音乐课专用控件：两条轨道 + 判定线 + 判定垫 + 金币池（初始隐藏）=====
static lv_obj_t* mus_make_dot(lv_obj_t* parent, int w, int h, uint32_t color, int radius) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static void create_music_ui(lv_obj_t* root) {
    // 轨道底线（判定线）：两条短黄线，位于各轨道底部
    for (int i = 0; i < 2; ++i) {
        s.mus_line[i] = mus_make_dot(root, kMusCoinSize + 8, 3, 0xF5A623, 0);
        lv_obj_set_pos(s.mus_line[i], kMusLaneX[i] - 4, kMusHitY);
    }
    // 判定垫：轨道下方可点区域（命中闪光用）
    for (int i = 0; i < 2; ++i) {
        s.mus_pad[i] = mus_make_dot(root, kMusCoinSize + 12, 16, 0x404040, 6);
        lv_obj_set_pos(s.mus_pad[i], kMusLaneX[i] - 6, kMusHitY + 4);
    }
    // 金币池：亮橙金 + 深色描边（浅绿背景上更醒目）
    for (int i = 0; i < kMusCoinPool; ++i) {
        s.mus_coin[i] = mus_make_dot(root, kMusCoinSize, kMusCoinSize, 0xFFB300,
                                     LV_RADIUS_CIRCLE);
        if (s.mus_coin[i]) {
            lv_obj_set_style_border_color(s.mus_coin[i], lv_color_hex(0x6E3B00), 0);
            lv_obj_set_style_border_width(s.mus_coin[i], 2, 0);
        }
    }
}

// 音乐控件显示/隐藏（进入音乐课显示轨道与判定垫）
static void music_ui_visible(bool show) {
    for (auto* o : s.mus_line) if (o) { if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
    for (auto* o : s.mus_pad)  if (o) { if (show) lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN); else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN); }
    if (!show) for (auto* o : s.mus_coin) if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// 计数器专用控件：算珠画布 + 每列位数字 + 右上迷你宠物（初始隐藏）。
// 两画布独立分配、独立容错：任一失败不影响其余控件。
static void create_counter_ui(lv_obj_t* root) {
    s.ctr_cbuf = (lv_color_t*)heap_caps_malloc(kCtrCanvasW * kCtrCanvasH * sizeof(lv_color_t),
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.ctr_cbuf) {
        s.ctr_canvas = lv_canvas_create(root);
        lv_canvas_set_buffer(s.ctr_canvas, s.ctr_cbuf, kCtrCanvasW, kCtrCanvasH,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s.ctr_canvas, kCtrCanvasW, kCtrCanvasH);
        lv_obj_set_pos(s.ctr_canvas, 0, 80);
        lv_obj_set_style_bg_opa(s.ctr_canvas, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(s.ctr_canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s.ctr_canvas, game_canvas_free_cb, LV_EVENT_DELETE, s.ctr_cbuf);
        lv_obj_add_flag(s.ctr_canvas, LV_OBJ_FLAG_HIDDEN);
    }

    // 每列位数字行（画布上方；中心对齐列；左→右 = 亿→个）
    for (int i = 0; i < 9; ++i) {
        s.ctr_digits[i] = lv_label_create(root);
        lv_label_set_text(s.ctr_digits[i], "0");
        lv_obj_set_style_text_color(s.ctr_digits[i], lv_color_hex(0x80A0B8), 0);
        lv_obj_set_style_text_font(s.ctr_digits[i], ui_font_16, 0);
        lv_obj_set_pos(s.ctr_digits[i], kCtrColX + i * kCtrColStep - 12, 56);
        lv_obj_set_size(s.ctr_digits[i], 24, 20);
        lv_obj_set_style_text_align(s.ctr_digits[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s.ctr_digits[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 每列下方竖排位名（画布下空出 184~216 区；左→右 = 亿→个；黑色）
    for (int i = 0; i < 9; ++i) {
        s.ctr_names[i] = lv_label_create(root);
        lv_label_set_text(s.ctr_names[i], kDigitVertNames[8 - i]);   // \n 竖排两字位名
        lv_label_set_long_mode(s.ctr_names[i], LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_color(s.ctr_names[i], COL_TEXT, 0);
        lv_obj_set_style_text_font(s.ctr_names[i], ui_font_16, 0);
        lv_obj_set_pos(s.ctr_names[i], kCtrColX + i * kCtrColStep - 8, 184);
        lv_obj_set_size(s.ctr_names[i], 16, 32);
        lv_obj_set_style_text_align(s.ctr_names[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_add_flag(s.ctr_names[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 右上迷你宠物（28x28，避开数字行；见 render_mini_pet_locked）
    s.ctr_pet_cbuf = (lv_color_t*)heap_caps_malloc(kMiniW * kMiniH * sizeof(lv_color_t),
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s.ctr_pet_cbuf) {
        s.ctr_pet = lv_canvas_create(root);
        lv_canvas_set_buffer(s.ctr_pet, s.ctr_pet_cbuf, kMiniW, kMiniH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_size(s.ctr_pet, kMiniW, kMiniH);
        lv_obj_set_pos(s.ctr_pet, 208, 26);
        lv_obj_set_style_bg_opa(s.ctr_pet, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(s.ctr_pet, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(s.ctr_pet, game_canvas_free_cb, LV_EVENT_DELETE, s.ctr_pet_cbuf);
        lv_obj_add_flag(s.ctr_pet, LV_OBJ_FLAG_HIDDEN);
    }
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

    // 计数器专用控件 + 布局切换（计数器：隐藏大宠物/题面/选项）
    create_counter_ui(root);
    // 音乐课专用控件（轨道/判定线/判定垫/金币池）
    create_music_ui(root);
    if (s.mode == Mode::Music) {
        // 音乐课：隐藏选项按钮，题面行让位给玩法；命中反馈放到底部（不挡金币）
        for (auto& o : s.opt) if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.q_label, LV_OBJ_FLAG_HIDDEN);
        music_ui_visible(true);
        if (s.hint_label) lv_label_set_text(s.hint_label, "左/右键在线上打点 长按退出");
        if (s.result_label) {
            lv_obj_set_pos(s.result_label, 0, 194);   // 判定垫下方，避免与下落金币重叠
            lv_label_set_text(s.result_label, " ");
        }
    }
    if (s.mode == Mode::Counter) {
        if (s.pet_canvas) lv_obj_add_flag(s.pet_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s.q_label, LV_OBJ_FLAG_HIDDEN);
        for (auto& o : s.opt) if (o) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s.result_label, 0, 28);
        lv_obj_set_size(s.result_label, 240, 20);
        if (s.ctr_canvas) lv_obj_clear_flag(s.ctr_canvas, LV_OBJ_FLAG_HIDDEN);
        for (auto& d : s.ctr_digits) if (d) lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
        for (auto& n : s.ctr_names)  if (n) lv_obj_clear_flag(n, LV_OBJ_FLAG_HIDDEN);
        if (s.ctr_pet) lv_obj_clear_flag(s.ctr_pet, LV_OBJ_FLAG_HIDDEN);
        // Intro 阶段即显示空算盘 + 全 0 位数字（避免画布初始为随机像素）
        if (s.ctr_canvas) {
            draw_counter_beads_locked();
            update_counter_digits_locked();
        }
    }
    return root;
}

// tick：Feedback 超时→下一题；Show 播放序列；Read 自动结束（需持锁部分内部处理）
// ===== 音乐课流程（下落金币打点，需持锁部分内部处理）=====
static void music_begin_locked() {
    int64_t now = esp_timer_get_time() / 1000;
    int64_t t0 = now + 1500;                      // 1.5s 预备
    for (int i = 0; i < kMusNotes; ++i) {
        s.mus_notes[i].lane   = (int)(esp_random() % 2);
        s.mus_notes[i].hit_ms = t0 + (int64_t)i * kMusBeatMs;
        s.mus_notes[i].state  = 0;
    }
    s.mus_next_beat = t0;
    s.mus_beat_idx  = 0;
    s.mus_score     = 0;
    s.mus_done      = 0;
    s.mus_pad_until[0] = s.mus_pad_until[1] = 0;
    for (auto* c : s.mus_coin) if (c) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    for (auto& ow : s.mus_slot_owner) ow = -1;   // 清槽位归属
    if (s.result_label) lv_label_set_text(s.result_label, "准备…");
    char b[24];
    snprintf(b, sizeof(b), "得分 0/%d", kMusNotes);
    if (s.info_label)   lv_label_set_text(s.info_label, b);
}

// 命中判定：在窗口内找同轨道最近的待判音符
static void music_hit(int lane) {
    if (lane < 0 || lane > 1) return;
    int64_t now = esp_timer_get_time() / 1000;
    int best = -1;
    int64_t best_dt = kMusHitWindow + 1;
    for (int i = 0; i < kMusNotes; ++i) {
        const auto& n = s.mus_notes[i];
        if (n.state != 0 || n.lane != lane) continue;
        int64_t dt = n.hit_ms > now ? (n.hit_ms - now) : (now - n.hit_ms);
        if (dt <= kMusHitWindow && dt < best_dt) { best = i; best_dt = dt; }
    }
    if (!lvgl_port_lock(100)) return;
    if (best >= 0) {
        s.mus_notes[best].state = 1;
        s.mus_score++;
        s.mus_done++;
        int slot = best % kMusCoinPool;
        if (s.mus_slot_owner[slot] == (int8_t)best) {
            lv_obj_t* c = s.mus_coin[slot];
            if (c) lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
            s.mus_slot_owner[slot] = -1;
        }
        // 判定垫绿色闪光
        if (s.mus_pad[lane]) {
            lv_obj_set_style_bg_color(s.mus_pad[lane], COL_OK, 0);
            lv_obj_set_style_bg_opa(s.mus_pad[lane], LV_OPA_COVER, 0);
            s.mus_pad_until[lane] = now + 120;
        }
        bsp::audio_play_tone(1319, 55);           // 命中高音反馈
    } else {
        bsp::audio_play(bsp::Sound::Wrong);       // 空按：不扣分
    }
    char b[24];
    snprintf(b, sizeof(b), "得分 %d/%d", s.mus_score, kMusNotes);
    if (s.info_label) lv_label_set_text(s.info_label, b);
    if (s.result_label) {
        lv_label_set_text(s.result_label, best >= 0 ? "命中!" : "…");
        lv_obj_set_style_text_color(s.result_label, best >= 0 ? COL_OK : COL_TEXT, 0);
    }
    lvgl_port_unlock();
}

// 每 tick：伴奏节拍 + 金币下落 + 漏判 + 结束（需持锁部分内部处理）
static void music_tick(int64_t now_ms) {
    // 有节奏的伴奏：每拍一个五声音阶音符（与金币同一节拍）
    static const int kMelody[8] = {523, 587, 659, 784, 880, 784, 659, 587};
    while (now_ms >= s.mus_next_beat) {
        bsp::audio_play_tone(kMelody[s.mus_beat_idx % 8], 150);
        s.mus_beat_idx++;
        s.mus_next_beat += kMusBeatMs;
    }

    if (!lvgl_port_lock(50)) return;
    // 金币位置 + 漏判
    // 槽位归属：金币对象复用（i % 池大小），但只有"当前可见的那个音符"能
    // 控制该槽位——否则远处的未来音符会在同一 tick 的 else 分支里把正在
    // 下落的金币又隐藏掉（60 音符 8 槽互相覆盖 → 全部看不见）。
    for (int i = 0; i < kMusNotes; ++i) {
        auto& n = s.mus_notes[i];
        if (n.state != 0) continue;
        int slot = i % kMusCoinPool;
        lv_obj_t* c = s.mus_coin[slot];
        if (!c) continue;
        int64_t dt = n.hit_ms - now_ms;                 // 距判定时刻
        bool visible = (dt <= kMusFallMs && dt > -300);
        if (visible) {
            s.mus_slot_owner[slot] = (int8_t)i;         // 认领槽位
            int y = kMusHitY - (int)(dt * (kMusHitY - kMusTopY) / kMusFallMs);
            lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_pos(c, kMusLaneX[n.lane], y);
        } else if (s.mus_slot_owner[slot] == (int8_t)i) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);     // 只隐藏自己占用的槽位
            s.mus_slot_owner[slot] = -1;
        }
        if (now_ms > n.hit_ms + kMusHitWindow) {        // 漏掉：不出分
            n.state = 2;
            s.mus_done++;
            if (s.mus_slot_owner[slot] == (int8_t)i) {
                lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
                s.mus_slot_owner[slot] = -1;
            }
        }
    }
    // 判定垫闪光复位
    for (int i = 0; i < 2; ++i) {
        if (s.mus_pad[i] && s.mus_pad_until[i] && now_ms >= s.mus_pad_until[i]) {
            lv_obj_set_style_bg_color(s.mus_pad[i], lv_color_hex(0x404040), 0);
            lv_obj_set_style_bg_opa(s.mus_pad[i], LV_OPA_COVER, 0);
            s.mus_pad_until[i] = 0;
        }
    }
    lvgl_port_unlock();

    // 结束：全部判定完 + 尾音结束
    if (s.mus_done >= kMusNotes
        && now_ms > s.mus_notes[kMusNotes - 1].hit_ms + 600) {
        finish_game();
    }
}

static void game_logic_tick(int64_t now_ms) {
    switch (s.phase) {
        case Phase::Show:
            if (s.mode == Mode::Music) {   // 音乐课：下落金币玩法
                music_tick(now_ms);
                return;
            }
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
                        lv_label_set_text(s.q_label,
                            (s.mode == Mode::Rhythm) ? "轮到你！按 左/中/右"
                                                     : "轮到你！按 左/右");
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
    // 跟拍记忆（节奏）：高亮当前步的轨道（三键三轨）
    // 音乐课不用此段（下落金币在 music_tick 里驱动）
    if (s.phase == Phase::Show && s.mode == Mode::Rhythm) {
        int step = s.show_step;
        bool flash = ((now_ms / 300) % 2) == 0;
        int lit = (step < kQuestions) ? s.seq[step] : -1;
        for (int i = 0; i < 3; ++i) {
            lv_obj_t* o = s.opt[i];
            if (!o) continue;
            bool on = (i == lit);
            lv_obj_set_style_bg_opa(o, (on && flash) ? LV_OPA_COVER : LV_OPA_30, 0);
            lv_obj_set_style_border_color(o, on ? COL_FOCUS : COL_TEXT, 0);
        }
        char b[24];
        snprintf(b, sizeof(b), "看仔细 %d/%d", step + 1, kQuestions);
        lv_label_set_text(s.q_label, b);
    } else {
        if (s.opt[0]) lv_obj_set_style_bg_opa(s.opt[0], LV_OPA_30, 0);
        if (s.opt[1]) lv_obj_set_style_bg_opa(s.opt[1], LV_OPA_30, 0);
        if (s.opt[2]) lv_obj_set_style_bg_opa(s.opt[2], LV_OPA_30, 0);
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
            case game::EduKind::Math:    s.mode = Mode::Math;    break;
            case game::EduKind::Music:   s.mode = Mode::Music;   break;
            case game::EduKind::Read:    s.mode = Mode::Read;    break;
            case game::EduKind::Counter: s.mode = Mode::Counter; break;
            default:                     s.mode = Mode::Word;    break;
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
    // 30Hz tick：音乐课金币下落需要平滑（原 10Hz 太顿挫）
    esp_timer_create_args_t cfg = {
        .callback = tick_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "game_tick",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &g_tick_timer);
    esp_timer_start_periodic(g_tick_timer, 33000ULL);
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
