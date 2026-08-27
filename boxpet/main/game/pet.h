// pet.h — 宠物核心（需求 v2：7 属性 + 状态机 + 互动 + 成长 + 繁育 + 事件）
#pragma once

#include <cstdint>
#include <vector>
#include <functional>

#include "pet_def.h"
#include "pet_event.h"

namespace boxpet::game {

constexpr int kLogMax = 8;   // 事件日志条数

struct LogEntry {
    int64_t     pet_sec;   // 事发宠物秒
    uint8_t     type;      // EventKind（截断）
};

struct PetState {
    TimeMode       time_mode   = TimeMode::Demo;
    Stage          stage       = Stage::Egg;
    EvoForm        evo_form    = EvoForm::None;
    PetStateKind   pstate      = PetStateKind::IDLE;

    // ===== 7 核心属性（float 累积，显示取整）=====
    float          hunger      = 70.0f;    // 饱食 0..100
    float          mood        = 70.0f;    // 心情 0..100
    float          energy      = 90.0f;    // 精力 0..100
    float          hygiene     = 80.0f;    // 卫生 0..100
    float          health      = 90.0f;    // 健康 0..100
    int            intelligence= 10;       // 智力 0..999
    int            bond        = 20;       // 亲密度 0..999

    // ===== 等级/经验 =====
    int            level       = 1;
    int            exp         = 0;

    // ===== 计时（真实秒驱动）=====
    int64_t        real_seconds = 0;    // 累计真实秒
    int64_t        pet_seconds  = 0;    // 累计宠物秒
    int64_t        egg_seconds  = 0;    // 蛋孵化计时（真实秒）
    int            age_pet_days = 0;    // 宠物日龄

    // ===== 状态计时（宠物秒）=====
    int64_t        state_since_pet_sec = 0;   // 当前状态进入时刻
    int64_t        sick_since_pet_sec  = -1;  // 生病起点（-1 未病）
    int64_t        dying_since_pet_sec = -1;  // 濒死起点
    int64_t        overeat_until_pet_sec = -1;// 吃撑截止
    int64_t        immunity_until_pet_sec = -1; // 维生素免疫截止
    int64_t        gestation_end_pet_sec = -1; // 孕育截止
    int64_t        gestation_start_pet_sec = -1;

    // ===== 日常计数（宠物日切换时重置）=====
    int            edu_count_today   = 0;
    int            rhythm_count_today= 0;
    int            snack_streak      = 0;   // 连续零食计数
    int            play_streak       = 0;   // 连续玩耍计数

    // ===== 冷却（宠物秒）=====
    int64_t        food_cooldown_pet_sec[(int)FoodKind::Count] = {0};

    // ===== 成长统计（进化判定用）=====
    float          mood_sum     = 0;     // 心情累计（每 tick 累加）
    float          hygiene_sum  = 0;
    int            mood_ticks   = 0;
    int            hygiene_ticks= 0;
    int            play_count   = 0;     // 累计玩耍次数
    int            feed_count   = 0;     // 累计喂食次数
    int            feed_on_time = 0;     // 规律喂食次数（hunger<50 时喂）
    int            perfect_streak_pet_days = 0;  // 全属性≥50 连续天数
    bool           dipped_below_50_today  = false;

    // ===== 便便 =====
    int            poop = 0;
    float          poop_accum = 0;      // 排泄累积器

    // ===== 抚摸 =====
    int            pet_count_depressed = 0;  // 抑郁中抚摸计数
    int            pet_count_sleeping  = 0;  // 睡眠中点击计数（连点 5 次强制唤醒）

    // ===== 背包 =====
    uint8_t        food_inv[(int)FoodKind::Count] = {5, 2, 0, 1, 0};
    uint8_t        med_inv[(int)MedKind::Count]   = {1, 0, 1, 0};

    // ===== 技能 =====
    uint8_t        skills = 0;    // SkillId 位掩码

    // ===== 繁育/世代 =====
    int            breed_count = 0;      // 已繁育次数
    int            babies_total = 0;     // 累计子女
    int            pending_eggs = 0;     // 待孵化蛋（死亡后可开新代）
    int            generation  = 1;
    int            inherit_int = 0;      // 后代继承智力
    uint8_t        inherit_skills = 0;   // 后代继承技能

    // ===== 灯光 =====
    bool           light_on = true;

    // ===== 特殊事件 =====
    SpecialEventId active_event = SpecialEventId::None;
    int64_t        event_deadline_real_sec = 0;  // 选择截止（真实秒）
    int64_t        last_interaction_pet_sec = 0; // 最近互动（bond 衰减用）
    int64_t        last_event_pet_sec = 0;       // 上次事件（节流）
    int64_t        auto_sleep_deadline_real = 0; // 23:00 提示后 30s 自动入睡
    bool           sleep_hint_shown = false;     // 本睡眠时段已提示

    // ===== 日志 =====
    LogEntry       log[kLogMax];
    int            log_head = 0;
    int            log_count = 0;

    // 存档 CRC（storage 层写）
    uint32_t       crc = 0;
};

using EventSink = std::function<void(const Event&)>;

class PetCore {
public:
    PetCore();

    // ===== 主 tick：每真实秒调用 =====
    void tick_real_second();

    // ===== 互动 =====
    void feed(FoodKind k);              // 喂食
    void pet_touch();                   // 抚摸
    void bathe();                       // 洗澡
    void medicate(MedKind k);           // 用药
    void toggle_light();                // 关灯/开灯（关灯→入睡）
    void request_sleep();               // 主动休息
    void force_wake();                  // 强制唤醒（连点）

    // ===== 玩耍/教育（结果由 UI 回传）=====
    bool can_play(PlayKind k, int* why = nullptr);
    void play_begin(PlayKind k);
    void play_end(PlayKind k, bool won);
    bool can_learn(EduKind k, int* why = nullptr);
    void edu_begin(EduKind k);
    void edu_end(EduKind k, int correct);

    // ===== 繁育 =====
    bool can_breed(int* why = nullptr);
    void breed_attempt();               // 相亲（AI 配种）

    // ===== 特殊事件 =====
    void resolve_event(int choice);     // choice 0=左 1=右
    SpecialEventId active_event() const { return s_.active_event; }

    // ===== 重置 =====
    void reset_to_new_egg();            // 死亡后：优先孵化待孵蛋（继承属性）

    // ===== 查询 =====
    const PetState& state() const { return s_; }
    int  hunger_pct() const { return (int)(s_.hunger + 0.5f); }
    int  mood_pct()   const { return (int)(s_.mood + 0.5f); }
    bool is_sleeping() const { return s_.pstate == PetStateKind::SLEEPING; }
    bool has_skill(SkillId id) const { return (s_.skills >> (int)id) & 1; }
    void log_add(uint8_t type);

    void load_state(const PetState& st) { s_ = st; }
    void subscribe(EventSink sink) { sinks_.push_back(std::move(sink)); }

    // 商店用：清除指定食物冷却（清到 0，下次可直接喂）
    void clear_food_cooldown(FoodKind k) {
        s_.food_cooldown_pet_sec[(int)k] = 0;
    }

private:
    PetState s_;
    std::vector<EventSink> sinks_;
    int      tick_accum_sec_ = 0;    // 60s 游戏节拍累积
    int      last_attn_bits_ = 0;
    int64_t  last_tick_us_   = 0;    // 上次 tick 的 esp_timer 时刻（补跳基准）

    // tick 内部
    void tick_one_second();         // 单秒推进（补跳循环体）
    void game_tick();                // 60s 一次：属性衰减/恢复/联动
    void advance_time();             // 每秒：宠物秒、日、阶段、状态超时
    void check_stage_evolution();
    void check_state_transitions();
    void check_death();
    void check_special_events();
    void trigger_event(SpecialEventId id);
    void finish_gestation();
    void apply_attention();
    void add_exp(int amount);
    bool try_learn_skill();
    void clamp_stats();
    void emit(EventKind k, int v1 = 0, int v2 = 0);
};

}  // namespace boxpet::game
