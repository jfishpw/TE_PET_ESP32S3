// ui_status.cpp — 状态页（需求 §2.6）
// 3 分页：左右翻页键切页；中键返回主界面
//   页 1 属性：健康/饱食/心情/精力/卫生（数值 + 文本进度条）
//   页 2 成长：等级/经验、智力、亲密度、阶段/进化、年龄、世代/宝宝
//   页 3 日志：技能 + 最近事件记录
#include "ui_status.h"
#include "ui_font_16.h"
#include "bsp/board.h"
#include "bsp/buttons.h"
#include "bsp/audio.h"
#include "game/pet_def.h"
#include "game/pet_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <cstdio>

static const char* TAG = "ui_status";

namespace boxpet::ui {

namespace {

constexpr int kPages = 3;
struct State {
    lv_obj_t* root = nullptr;
    // 页 1：5 条属性（名称+进度条+数值）
    lv_obj_t* stat_lbl[5]   = {nullptr};
    // 页 2：6 行成长信息
    lv_obj_t* grow_lbl[6]   = {nullptr};
    // 页 3：技能 + 6 行日志
    lv_obj_t* skill_lbl     = nullptr;
    lv_obj_t* log_lbl[6]    = {nullptr};
    lv_obj_t* page_hint     = nullptr;
    int       page = 0;
    ::boxpet::game::PetCore* pet = nullptr;
    bool      wants_leave = false;
};
static State s;
static esp_timer_handle_t s_tick = nullptr;

static const lv_color_t COL_BG     = lv_color_hex(0xC0DCC0);
static const lv_color_t COL_TEXT   = lv_color_hex(0x202020);
static const lv_color_t COL_BAR    = lv_color_hex(0x404040);
static const lv_color_t COL_WHITE  = lv_color_hex(0xFFFFFF);
static const lv_color_t COL_WARN   = lv_color_hex(0xC00000);

// ===== 名称辅助 =====
static const char* stage_name(::boxpet::game::Stage st) {
    using namespace ::boxpet::game;
    switch (st) {
        case Stage::Egg:      return "蛋";
        case Stage::Baby:     return "幼年";
        case Stage::Juvenile: return "少年";
        case Stage::Adult:    return "成熟";
        case Stage::Senior:   return "老年";
        default:              return "-";
    }
}
static const char* evo_name(::boxpet::game::EvoForm f) {
    using namespace ::boxpet::game;
    switch (f) {
        case EvoForm::Normal:   return "普通型";
        case EvoForm::Scholar:  return "学者型";
        case EvoForm::Active:   return "活力型";
        case EvoForm::Graceful: return "优雅型";
        case EvoForm::Radiant:  return "光辉型";
        default:                return "未定";
    }
}
static const char* pstate_name(::boxpet::game::PetStateKind p) {
    using namespace ::boxpet::game;
    switch (p) {
        case PetStateKind::IDLE:      return "正常";
        case PetStateKind::EATING:    return "进食";
        case PetStateKind::SLEEPING:  return "睡眠";
        case PetStateKind::PLAYING:   return "玩耍";
        case PetStateKind::LEARNING:  return "学习";
        case PetStateKind::SICK:      return "生病";
        case PetStateKind::DEAD:      return "死亡";
        case PetStateKind::DEPRESSED: return "抑郁";
        case PetStateKind::BATHING:   return "洗澡";
        case PetStateKind::EVOLVING:  return "进化";
        case PetStateKind::BREEDING:  return "孕育";
        default:                      return "-";
    }
}
// 日志事件名（按 EventKind）
static const char* event_name(uint8_t type) {
    using K = ::boxpet::game::EventKind;
    switch ((K)type) {
        case K::Hatch:          return "孵化了";
        case K::StageChanged:   return "成长了";
        case K::EvoDecided:     return "进化定型";
        case K::Sick:           return "生病了";
        case K::Healed:         return "痊愈了";
        case K::Overeat:        return "吃撑了";
        case K::Depressed:      return "情绪低落";
        case K::DepressCured:   return "心情好转";
        case K::Died:           return "去世了";
        case K::Dying:          return "非常危险";
        case K::Born:           return "宝宝出生";
        case K::GestationStart: return "有宝宝了";
        case K::FeedOk:         return "喂食";
        case K::BatheOk:        return "洗澡";
        case K::MedOk:          return "用药";
        case K::PlayFinished:   return "玩耍";
        case K::EduFinished:    return "学习";
        case K::SkillLearned:   return "学会技能";
        case K::LevelUp:        return "升级";
        case K::SpecialEvent:   return "特殊事件";
        case K::EventResolved:  return "事件结束";
        case K::SleepStart:     return "入睡";
        case K::WakeUp:         return "醒来";
        case K::GiftReceived:   return "收到礼物";
        default:                return "记录";
    }
}

// 文本进度条：10 格
static void stat_bar(char* buf, size_t cap, float v) {
    int filled = (int)(v / 10.0f + 0.5f);
    if (filled < 0) filled = 0;
    if (filled > 10) filled = 10;
    int p = 0;
    buf[p++] = ' ';
    for (int i = 0; i < 10; ++i) buf[p++] = (i < filled) ? '#' : '-';
    buf[p] = 0;
    (void)cap;
}

static void refresh() {
    using namespace ::boxpet::game;
    if (!s.pet) return;
    if (!lvgl_port_lock(100)) return;
    const PetState& st = s.pet->state();
    char buf[48];

    // ===== 页 1：五条属性 =====
    {
        struct { const char* name; float v; } rows[5] = {
            {"健康", st.health}, {"饱食", st.hunger}, {"心情", st.mood},
            {"精力", st.energy}, {"卫生", st.hygiene},
        };
        for (int i = 0; i < 5; ++i) {
            char bar[16];
            stat_bar(bar, sizeof(bar), rows[i].v);
            snprintf(buf, sizeof(buf), "%s%s %3d", rows[i].name, bar,
                     (int)(rows[i].v + 0.5f));
            lv_label_set_text(s.stat_lbl[i], buf);
            // 低值红色警示
            lv_obj_set_style_text_color(s.stat_lbl[i],
                rows[i].v < 20 ? COL_WARN : COL_TEXT, 0);
        }
    }

    // ===== 页 2：成长/繁育 =====
    snprintf(buf, sizeof(buf), "等级 Lv%d  经验 %d", st.level, st.exp);
    lv_label_set_text(s.grow_lbl[0], buf);
    snprintf(buf, sizeof(buf), "智力 %d   亲密 %d", st.intelligence, st.bond);
    lv_label_set_text(s.grow_lbl[1], buf);
    snprintf(buf, sizeof(buf), "阶段 %s  进化 %s", stage_name(st.stage),
             evo_name(st.evo_form));
    lv_label_set_text(s.grow_lbl[2], buf);
    snprintf(buf, sizeof(buf), "年龄 第%d天  状态 %s", st.age_pet_days,
             pstate_name(st.pstate));
    lv_label_set_text(s.grow_lbl[3], buf);
    snprintf(buf, sizeof(buf), "世代 第%d代  宝宝 %d", st.generation,
             st.babies_total);
    lv_label_set_text(s.grow_lbl[4], buf);
    // 第 5 行：吃撑/免疫/孕育 debuff 提示
    const char* buff = "";
    if (st.gestation_end_pet_sec > 0)      buff = "孕育中…";
    else if (st.overeat_until_pet_sec > 0) buff = "吃撑中…";
    else if (st.immunity_until_pet_sec > 0)buff = "免疫中…";
    else if (st.poop > 0)                  buff = "有便便要清理";
    snprintf(buf, sizeof(buf), "%s", buff[0] ? buff : "一切正常");
    lv_label_set_text(s.grow_lbl[5], buf);

    // ===== 页 3：技能 + 日志 =====
    {
        char sk[40];
        int p = 0;
        p += snprintf(sk + p, sizeof(sk) - p, "技能:");
        if (st.skills == 0) {
            p += snprintf(sk + p, sizeof(sk) - p, " 暂无");
        } else {
            for (int i = 0; i < (int)SkillId::Count && p < 34; ++i) {
                if ((st.skills >> i) & 1)
                    p += snprintf(sk + p, sizeof(sk) - p, " %s", kSkillNames[i]);
            }
        }
        lv_label_set_text(s.skill_lbl, sk);

        int shown = 0;
        for (int i = 0; i < st.log_count && shown < 6; ++i) {
            int idx = (st.log_head - 1 - i + kLogMax * 2) % kLogMax;
            const auto& e = st.log[idx];
            int day = (int)(e.pet_sec / seconds_per_pet_day(st.time_mode));
            snprintf(buf, sizeof(buf), "第%d天 %s", day, event_name(e.type));
            lv_label_set_text(s.log_lbl[shown], buf);
            ++shown;
        }
        for (; shown < 6; ++shown) lv_label_set_text(s.log_lbl[shown], "");
    }

    // 分页提示
    static const char* page_names[kPages] = {"1/3 属性", "2/3 成长", "3/3 日志"};
    if (s.page_hint) lv_label_set_text(s.page_hint, page_names[s.page]);
    lvgl_port_unlock();
}

// 按 page 显示/隐藏
static void apply_page_visibility() {
    if (!lvgl_port_lock(100)) return;
    // 页 1（属性）：5 条属性全部显示；其他页全部隐藏
    // （修复：原代码把"标签下标 == 页码"当成显示条件，
    //   导致属性页只剩 1 行、其余 4 行混入成长/日志页与其他文字重叠）
    for (int i = 0; i < 5; ++i) {
        if (s.page == 0) lv_obj_clear_flag(s.stat_lbl[i], LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag(s.stat_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < 6; ++i) {
        if (s.page == 1) lv_obj_clear_flag(s.grow_lbl[i], LV_OBJ_FLAG_HIDDEN);
        else             lv_obj_add_flag(s.grow_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s.page == 2) {
        lv_obj_clear_flag(s.skill_lbl, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 6; ++i) lv_obj_clear_flag(s.log_lbl[i], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s.skill_lbl, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 6; ++i) lv_obj_add_flag(s.log_lbl[i], LV_OBJ_FLAG_HIDDEN);
    }
    lvgl_port_unlock();
}

static void on_key(bsp::KeyId id, bsp::KeyEvent evt) {
    if (evt == bsp::KeyEvent::ShortPress) {
        if (id == bsp::KeyId::Left)  { s.page = (s.page + kPages - 1) % kPages; bsp::audio_play(bsp::Sound::Tick); apply_page_visibility(); refresh(); }
        if (id == bsp::KeyId::Right) { s.page = (s.page + 1) % kPages; bsp::audio_play(bsp::Sound::Tick); apply_page_visibility(); refresh(); }
        if (id == bsp::KeyId::Mid)   { s.wants_leave = true; bsp::audio_play(bsp::Sound::Beep); }
    } else if (evt == bsp::KeyEvent::LongPress && id == bsp::KeyId::Mid) {
        s.wants_leave = true;
    }
}

static lv_obj_t* make_txt(lv_obj_t* parent, int x, int y, int w) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, "");
    lv_obj_set_size(l, w, 22);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_color(l, COL_TEXT, 0);
    lv_obj_set_style_text_font(l, ui_font_16, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
    return l;
}

static lv_obj_t* build() {
    lv_obj_t* root = lv_obj_create(nullptr);
    lv_obj_set_size(root, 240, 240);
    lv_obj_set_style_bg_color(root, COL_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    // 顶栏：标题 + 页码
    lv_obj_t* top = lv_obj_create(root);
    lv_obj_set_size(top, 240, 24);
    lv_obj_set_pos(top, 0, 0);
    lv_obj_set_style_bg_color(top, COL_BAR, 0);
    lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
    lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(top, 0, 0);
    lv_obj_t* title = lv_label_create(top);
    lv_label_set_text(title, "状态");
    lv_obj_set_style_text_color(title, COL_WHITE, 0);
    lv_obj_set_style_text_font(title, ui_font_16, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
    s.page_hint = lv_label_create(top);
    lv_label_set_text(s.page_hint, "1/3 属性");
    lv_obj_set_style_text_color(s.page_hint, COL_WHITE, 0);
    lv_obj_set_style_text_font(s.page_hint, ui_font_16, 0);
    lv_obj_align(s.page_hint, LV_ALIGN_RIGHT_MID, -8, 0);

    // 页 1：属性 5 行
    for (int i = 0; i < 5; ++i)
        s.stat_lbl[i] = make_txt(root, 14, 36 + i * 30, 214);
    // 页 2：成长 6 行
    for (int i = 0; i < 6; ++i)
        s.grow_lbl[i] = make_txt(root, 14, 34 + i * 26, 214);
    // 页 3：技能 1 行 + 日志 6 行
    s.skill_lbl = make_txt(root, 14, 32, 214);
    for (int i = 0; i < 6; ++i)
        s.log_lbl[i] = make_txt(root, 14, 60 + i * 24, 214);

    // 底栏
    lv_obj_t* bot = lv_obj_create(root);
    lv_obj_set_size(bot, 240, 24);
    lv_obj_set_pos(bot, 0, 216);
    lv_obj_set_style_bg_color(bot, COL_BAR, 0);
    lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bot, 0, 0);
    lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* hint = lv_label_create(bot);
    lv_label_set_text(hint, "左右翻页 中返回");
    lv_obj_set_style_text_color(hint, COL_WHITE, 0);
    lv_obj_set_style_text_font(hint, ui_font_16, 0);
    lv_obj_center(hint);
    return root;
}

static void tick_cb(void* /*arg*/) {
    refresh();
}

}  // namespace

lv_obj_t* ui_status_create() {
    s.root = build();
    s.page = 0;
    bsp::buttons_set_callback(on_key);
    apply_page_visibility();
    refresh();
    if (!s_tick) {
        esp_timer_create_args_t cfg = {.callback = tick_cb, .dispatch_method = ESP_TIMER_TASK,
                                       .name = "status_tick", .skip_unhandled_events = true};
        esp_timer_create(&cfg, &s_tick);
        esp_timer_start_periodic(s_tick, 1000000ULL);  // 1s
    }
    return s.root;
}

void ui_status_set_pet(::boxpet::game::PetCore* pet) {
    s.pet = pet;
    refresh();
}

void ui_status_close() {
    if (s_tick) {
        esp_timer_stop(s_tick);
        esp_timer_delete(s_tick);
        s_tick = nullptr;
    }
    // 删除屏幕对象（修复泄漏：此前只置空指针，每次进出状态页泄漏 ~6KB，
    // 多次切换后耗尽 64KB LVGL 池 → lv_realloc 返回 NULL → 崩溃）
    if (s.root && lvgl_port_lock(200)) {
        lv_obj_delete_async(s.root);   // 异步删除，由 LVGL 任务在安全时机回收
        lvgl_port_unlock();
    }
    s.root = nullptr;
    s.pet = nullptr;
}

bool ui_status_wants_leave() { return s.wants_leave; }
void ui_status_clear_leave_flag() { s.wants_leave = false; }

}  // namespace boxpet::ui
