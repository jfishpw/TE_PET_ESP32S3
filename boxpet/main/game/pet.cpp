// pet.cpp — 宠物核心实现（需求 v2）
//   * tick_real_second() 每真实秒调用：推进时间 + 状态超时
//   * game_tick() 每 60 真实秒一次：属性衰减/恢复/联动/事件抽签
//   * 演示模式速率 ×24（1 宠物日 = 1 真实小时）
#include "pet.h"

#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace boxpet::game {

static const char* TAG = "pet";

static int rand_pct() { return (int)(esp_random() % 100); }

// 真实秒 → 宠物秒：两种模式下均 1:1
// （宠物日长度由 seconds_per_pet_day() 决定：演示 3600 秒/宠物日，真实 86400 秒/宠物日；
//   宠物小时 = 宠物日/24：演示 150 秒，真实 3600 秒）
static int64_t real_to_pet_sec(int64_t real_sec, TimeMode) {
    return real_sec;
}

// 事件超时默认选项（Runaway 超时 = 没追回）
static int event_default_choice(SpecialEventId id) {
    return id == SpecialEventId::Runaway ? 1 : 0;
}

PetCore::PetCore() {
    // 全新宠物（无存档路径）：蛋皮随机 1/4；有存档时 load_state 覆盖
    s_.evo_look = (uint8_t)(esp_random() % 4);
}

void PetCore::emit(EventKind k, int v1, int v2) {
    Event e{k, v1, v2};
    for (auto& s : sinks_) if (s) s(e);
}

void PetCore::log_add(uint8_t type) {
    s_.log[s_.log_head] = LogEntry{s_.pet_seconds, type};
    s_.log_head = (s_.log_head + 1) % kLogMax;
    if (s_.log_count < kLogMax) s_.log_count++;
}

void PetCore::clamp_stats() {
    if (s_.hunger  > kStatMax) s_.hunger  = kStatMax;
    if (s_.hunger  < kStatMin) s_.hunger  = kStatMin;
    if (s_.mood    > kStatMax) s_.mood    = kStatMax;
    if (s_.mood    < kStatMin) s_.mood    = kStatMin;
    if (s_.energy  > kStatMax) s_.energy  = kStatMax;
    if (s_.energy  < kStatMin) s_.energy  = kStatMin;
    if (s_.hygiene > kStatMax) s_.hygiene = kStatMax;
    if (s_.hygiene < kStatMin) s_.hygiene = kStatMin;
    if (s_.health  > kStatMax) s_.health  = kStatMax;
    if (s_.health  < kStatMin) s_.health  = kStatMin;
    if (s_.intelligence > kIntMax)  s_.intelligence = kIntMax;
    if (s_.intelligence < 0)        s_.intelligence = 0;
    if (s_.bond > kBondMax) s_.bond = kBondMax;
    if (s_.bond < 0)        s_.bond = 0;
}

// ===== 主 tick（每真实秒调用）=====
// Light Sleep 期间 esp_timer 时基由 RTC 补偿（IDF sleep_modes.c 用
// rtc_time_diff 把 esp_timer 拨到醒来时刻），但 pet_tick 定时器
// skip_unhandled_events=true → 睡眠醒来只补 1 次回调。若每次只推进 1 秒，
// 睡眠期间宠物时间冻结（睡 8 小时宠物只老 1 秒）。因此按 esp_timer 实际
// 流逝秒数补跳：正常 1Hz 调用 dt=1，睡眠醒来 dt=睡眠时长，逐秒推进。
// 注意：本任务（esp_timer 任务，优先级 22）补跳几千秒时会长时间独占 CPU，
// 低优先级任务（pm_sleep 优先级 1 等）会被饿死（曾触发 TWDT 误复位），
// 故每 32 秒让出 1ms。
void PetCore::tick_real_second() {
    if (s_.pstate == PetStateKind::DEAD) return;
    int64_t now_us = esp_timer_get_time();
    if (last_tick_us_ == 0) last_tick_us_ = now_us - 1000000;  // 首次调用
    int64_t dt = (now_us - last_tick_us_ + 500000) / 1000000;  // 四舍五入到秒
    if (dt <= 0) return;
    if (dt > 86400) dt = 86400;  // 防御上限：1 天
    last_tick_us_ = now_us;

    // ---- 补跳（dt>1，浅睡/长间隔醒来）保护 ----
    // 1) 补跳期间不触发随机事件：否则事件在补跳中途弹出（实测"唤醒后显示
    //    访客来了"），用户按键与补跳并发改宠物状态 → 卡死；
    // 2) 置 catchup_ 标志：UI 键处理在补跳期间忽略输入（见 ui_main on_key），
    //    从根上杜绝"按键任务 vs 补跳任务"并发读写 PetState。
    const bool catchup = (dt > 1);
    bool saved_events = events_enabled_;
    if (catchup) { events_enabled_ = false; catchup_ = true; }
    for (int64_t i = 0; i < dt; ++i) {
        if ((i & 0x1F) == 0) vTaskDelay(1);   // 每 32 秒让出 CPU（防饿死低优先级任务）
        tick_one_second();
    }
    if (catchup) {
        // 补跳完成：恢复事件开关，并补做补跳期间被禁用的随机事件抽签
        // （原节奏 = 每 60 宠物秒一次；只弹一个，避免醒来瞬间连弹）。
        // catchup_ 保持到补抽结束，期间按键仍被 UI 忽略（防并发）。
        events_enabled_ = saved_events;
        roll_events_after_catchup(dt);
        catchup_ = false;
    }
}

// 补跳后补抽随机事件（详见 pet.h 注释）。
// 注意：check_special_events 在 SLEEPING/已有活动事件时会直接返回，所以
// 夜间深睡醒来（宠物仍睡）不会被打扰；白天浅睡醒来则会正常遇到事件。
void PetCore::roll_events_after_catchup(int64_t elapsed_sec) {
    if (!events_enabled_) return;
    int rolls = (int)(elapsed_sec / 60);      // 每 60 宠物秒 1 次原抽签节奏
    if (rolls > 180) rolls = 180;             // 上限 3 小时量（防超长补跳后卡顿）
    for (int i = 0; i < rolls; ++i) {
        check_special_events();
        if (s_.active_event != SpecialEventId::None) break;   // 只弹一个
    }
}

void PetCore::tick_one_second() {
    if (s_.pstate == PetStateKind::DEAD) return;
    s_.real_seconds++;

    // 蛋：只累计孵化时间
    if (s_.stage == Stage::Egg) {
        s_.egg_seconds++;
        if (s_.egg_seconds >= egg_incubation_seconds(s_.time_mode)) {
            s_.stage = Stage::Baby;
            s_.age_pet_days = 0;
            s_.pstate = PetStateKind::IDLE;
            s_.state_since_pet_sec = s_.pet_seconds;
            // 孵化即首次分支判定：三维全 0 → 保底力量型（平局规则见 pet_def.h）；
            // LV2 升级进化时会按真实三维重新判定分支，可换向。
            s_.evo_branch  = evo_decide_branch(s_.evo_power, s_.evo_magic, s_.evo_speed);
            s_.evo_quality = evo_decide_quality(s_.evo_power, s_.evo_magic, s_.evo_speed);
            s_.evo_look    = (uint8_t)evo_look_for(Stage::Baby, s_.evo_branch);
            emit(EventKind::Hatch);
            emit(EventKind::StageChanged, (int)Stage::Baby);
            log_add((uint8_t)EventKind::Hatch);
            add_exp(kExpEvent);
        }
        // 事件截止检查（无）
        return;
    }

    // 宠物时间推进（宠物秒与真实秒 1:1；宠物日长度随模式压缩）
    s_.pet_seconds += real_to_pet_sec(1, s_.time_mode);

    // 宠物日切换
    int new_day = (int)(s_.pet_seconds / seconds_per_pet_day(s_.time_mode));
    if (new_day != s_.age_pet_days) {
        s_.age_pet_days = new_day;
        // 日常重置
        s_.edu_count_today = 0;
        s_.rhythm_count_today = 0;
        s_.play_streak = 0;
        // 亲密度闲置衰减
        if (s_.pet_seconds - s_.last_interaction_pet_sec
              > (int64_t)kBondIdlePetDays * seconds_per_pet_day(s_.time_mode)) {
            s_.bond -= kBondIdleLossPerDay;
        }
    }

    // 60s 游戏节拍
    if (++tick_accum_sec_ >= 60) {
        tick_accum_sec_ = 0;
        game_tick();
    }

    advance_time();
    apply_attention();
}

// ===== 60s 游戏节拍：属性变化 =====
void PetCore::game_tick() {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return;

    // 速率倍数：真实=1（每 tick = 1 分钟量），演示=24
    const float mult = (s_.time_mode == TimeMode::Demo) ? kDemoSpeedup : 1.0f;
    const bool sleeping = (s_.pstate == PetStateKind::SLEEPING);
    const bool sick = (s_.pstate == PetStateKind::SICK);

    if (sleeping) {
        // 睡眠：hunger 减半，mood/hygiene 冻结，energy 恢复（生病时恢复减半）
        float rec = kRateEnergyRecover * mult * (sick ? 0.5f : 1.0f);
        s_.energy += rec;
        s_.hunger -= kRateHungerDecay * kSleepHungerFactor * mult;
    } else {
        s_.hunger -= kRateHungerDecay * mult;
        s_.mood   -= kRateMoodDecay * mult;
        s_.hygiene-= kRateHygieneDecay * mult;
        s_.energy -= kRateEnergyDrain * mult;
    }

    // 联动：hunger=0 → health -2/h, mood -5/h
    if (s_.hunger <= 0) {
        s_.health -= kRateHealthStarve * mult;
        s_.mood   -= kRateMoodStarve * mult;
    }

    // 联动：hygiene≤20 → 每宠物小时 30% 生病
    if (s_.hygiene <= kSickHygieneThreshold
        && s_.pstate != PetStateKind::SICK
        && s_.immunity_until_pet_sec < s_.pet_seconds) {
        // 每 tick 抽一次（按 tick 时长占宠物小时比例缩放概率）
        float frac = 60.0f * mult / (float)(seconds_per_pet_day(s_.time_mode) / 24);
        if ((float)rand_pct() < kSickHygieneChancePerHour * frac) {
            s_.sick_since_pet_sec = s_.pet_seconds;
            s_.pstate = PetStateKind::SICK;
            s_.state_since_pet_sec = s_.pet_seconds;
            emit(EventKind::Sick);
            log_add((uint8_t)EventKind::Sick);
        }
    }

    // 排泄：平均 kPoopSpawnPerPetHour 个/宠物小时。本 tick 是 1 真实秒，
    // 1 宠物小时 = seconds_per_pet_day/24 真实秒，所以
    // 每 tick 增量 = kPoopSpawnPerPetHour / (seconds_per_pet_day/24)。
    //   真实模式：1/6 / (86400/24) = 1/21600 → 每 21600 秒 = 6 真实小时 1 个便便
    //   演示模式：1/6 / (3600/24)  = 1/900   → 每 900 秒    = 15 分钟演示时间 1 个便便
    if (!sleeping) {
        float real_sec_per_pet_hour = (float)seconds_per_pet_day(s_.time_mode) / 24.0f;
        float pet_hours_per_tick    = 1.0f / real_sec_per_pet_hour;
        s_.poop_accum += kPoopSpawnPerPetHour * pet_hours_per_tick;
        if (s_.poop_accum >= 1.0f) {
            s_.poop_accum -= 1.0f;
            if (s_.poop < kPoopMax) {
                s_.poop++;
                s_.hygiene -= kPoopHygienePenalty;
                emit(EventKind::Pooped, s_.poop);
            }
        }
    }

    clamp_stats();

    // 孕育到期
    if (s_.gestation_end_pet_sec > 0 && s_.pet_seconds >= s_.gestation_end_pet_sec) {
        finish_gestation();
    }

    // 特殊事件抽签
    check_special_events();
}

// ===== 阶段跃迁 + 分支进化（v5：等级+日龄双门槛，每次进化重判分支与品质）=====
// 双门槛（见 pet_def.h kEvoLv*/kEvoDay*）：等级、日龄都满足才进化；不满足则
// 每 tick 复查，条件齐的瞬间立即触发——升级快不会跳过阶段，玩得久也不会卡级。
// 进化要求 IDLE：睡觉/生病/动画中不打断（条件保持，醒来即进化）。
// 分支：power/magic/speed 取最大（平局优先 力>魔>速），每次进化可换向；
// 品质：三维和分档（<90 普通 / 90..150 优秀 / >150 华丽），UI 层渲染光效。
// 老年跃迁：成熟期/完全体日龄 ≥60 → 老年（寿命机制不变，允许睡眠中发生）。
void PetCore::check_stage_evolution() {
    // 老年：寿命到点，无演出门槛（睡眠中也照常变老）
    if ((s_.stage == Stage::Adult || s_.stage == Stage::Ultimate)
        && s_.age_pet_days >= kStageSeniorStartDay) {
        s_.stage    = Stage::Senior;
        s_.evo_look = (uint8_t)EvoLook::Senior;
        s_.pstate   = PetStateKind::EVOLVING;
        s_.state_since_pet_sec = s_.pet_seconds;
        emit(EventKind::StageChanged, (int)Stage::Senior);
        log_add((uint8_t)EventKind::StageChanged);
        add_exp(kExpEvent);
        return;
    }

    // 双门槛等级进化
    Stage target = s_.stage;
    switch (s_.stage) {
        case Stage::Baby:
            if (s_.level >= kEvoLvGrowth && s_.age_pet_days >= kEvoDayGrowth)
                target = Stage::Juvenile;
            break;
        case Stage::Juvenile:
            if (s_.level >= kEvoLvMature && s_.age_pet_days >= kEvoDayMature)
                target = Stage::Adult;
            break;
        case Stage::Adult:
            if (s_.level >= kEvoLvUltimate && s_.age_pet_days >= kEvoDayUltimate)
                target = Stage::Ultimate;
            break;
        default:
            break;   // 蛋期孵化在 tick_one_second；老年/死亡不进化
    }
    if (target == s_.stage) return;
    if (s_.pstate != PetStateKind::IDLE) return;   // 醒着才进化（条件保持）

    uint8_t old_look = s_.evo_look;
    s_.evo_branch  = evo_decide_branch(s_.evo_power, s_.evo_magic, s_.evo_speed);
    s_.evo_quality = evo_decide_quality(s_.evo_power, s_.evo_magic, s_.evo_speed);
    s_.evo_look    = (uint8_t)evo_look_for(target, s_.evo_branch);
    s_.stage    = target;
    s_.pstate   = PetStateKind::EVOLVING;                  // 4s 后回 IDLE（演出窗口）
    s_.state_since_pet_sec = s_.pet_seconds;
    // v1=新阶段(Stage) v2=旧外观ID（UI 播放"旧形态↔新形态"交替进化动画）
    emit(EventKind::EvolveStart, (int)target, old_look);
    log_add((uint8_t)EventKind::EvolveStart);
    add_exp(kExpEvent);
}

// ===== 状态转换（需求 §1.4 + 需求2 作息修复）=====
//
// 状态机（含需求2修复项）：
//
//            ┌────────────────────到起床点/energy满──自然醒(v1=4/2)───────────┐
//            │                                                                │
//   IDLE ──┬─┴─ energy=0 ──→ SLEEPING ←──睡眠窗内(真实钟)提示30s后强制入睡───┬─┴── SICK/DEPRESSED
//     │    │                   │   ▲                                        │
//     │    └─ mood≤10 → DEPRESSED │   └── 睡醒时若仍患病(sick_since≥0)→回 SICK ┘
//     │                           │
//     ├─ 吃/洗/演化(4s超时) → IDLE
//     ├─ 玩/学(120s兜底) → IDLE
//     └─ (抑郁满1宠物日 → 离家出走事件)
//
// 需求2 修复点：
//   1) 睡眠窗判断时钟：真实模式用注入的 wallclock 真实小时（原 pet_seconds
//      相对时钟从开机起算，与真实作息完全脱节 → "凌晨4点不睡"根因）；
//      演示模式仍用宠物时钟。
//   2) 入睡检查从仅 IDLE 扩展到 IDLE/SICK/DEPRESSED（原 SICK/DEPRESSED
//      状态下到点永不入睡）。
//   3) 自然醒：到起床小时点自动醒 + 发 WakeUp(v1=4)（UI 播放伸懒腰动画）；
//      睡醒时若仍患病（sick_since_pet_sec≥0，服药时才清 -1）→ 回 SICK，
//      杜绝"睡一觉病好了"漏洞。energy≥100 醒保留（v1=2）。
//   4) 时间与精力关系（明确统一）：睡眠窗内到点强制入睡（无视精力，时间优先）；
//      窗外 energy=0 也可随时入睡（或关系）。
void PetCore::check_state_transitions() {
    int64_t pet_hour = seconds_per_pet_day(s_.time_mode) / 24;
    (void)pet_hour;
    // 当前作息小时：真实模式优先用真实时钟（需求2 根因修复）
    int sched_hour;
    if (s_.time_mode == TimeMode::Real && real_hour_fn_) {
        sched_hour = real_hour_fn_();
    } else {
        sched_hour = pet_clock_from_seconds(s_.pet_seconds, s_.time_mode).hour;
    }
    const bool in_window = in_sleep_window(sched_hour);

    // 睡眠窗内到点强制入睡（IDLE 提示 30s；SICK/DEPRESSED 直接睡——
    // 它们无交互菜单，提示无意义，且病/抑郁中也要睡觉）
    auto try_auto_sleep = [&]() {
        if (!in_window) { s_.sleep_hint_shown = false; s_.auto_sleep_deadline_real = 0; return; }
        if (s_.pstate == PetStateKind::IDLE) {
            if (!s_.sleep_hint_shown) {
                s_.sleep_hint_shown = true;
                s_.auto_sleep_deadline_real = s_.real_seconds + kAutoSleepDelaySec;
                emit(EventKind::AutoSleepHint);
            } else if (s_.auto_sleep_deadline_real > 0
                       && s_.real_seconds >= s_.auto_sleep_deadline_real) {
                s_.auto_sleep_deadline_real = 0;
                enter_sleep(false /*手动=否*/);
            }
        } else {   // SICK / DEPRESSED：直接入睡
            enter_sleep(false);
        }
    };

    switch (s_.pstate) {
        case PetStateKind::EATING:
        case PetStateKind::BATHING:
        case PetStateKind::EVOLVING:
            // 3~5 真实秒后回 IDLE
            if (s_.pet_seconds - s_.state_since_pet_sec
                >= real_to_pet_sec(4, s_.time_mode)) {
                s_.pstate = PetStateKind::IDLE;
                s_.state_since_pet_sec = s_.pet_seconds;
            }
            break;
        case PetStateKind::PLAYING:
        case PetStateKind::LEARNING:
            // 安全兜底：2 分钟未回调 end → 回 IDLE
            if (s_.pet_seconds - s_.state_since_pet_sec
                >= real_to_pet_sec(120, s_.time_mode)) {
                s_.pstate = PetStateKind::IDLE;
                s_.state_since_pet_sec = s_.pet_seconds;
            }
            break;
        case PetStateKind::SLEEPING: {
            // 睡眠改写 v3：
            //   夜间（窗口内）：无论精力多少都**不自动醒**（起床靠白天）。到起床点
            //     若精力未恢复满，仍在窗内 → 继续睡；出窗且白天精力满 → 下方自动醒。
            //   白天（窗口外）：精力恢复满(100) → 自动起床 + 自动开灯（kind=2）。
            //   （手动开灯提前起床走 toggle_light，需白天+精力≥60。）
            if (!in_window && s_.energy >= kStatMax) {
                s_.pstate = (s_.sick_since_pet_sec >= 0) ? PetStateKind::SICK
                                                         : PetStateKind::IDLE;
                s_.state_since_pet_sec = s_.pet_seconds;
                s_.sleep_manual_ = false;
                exit_sleep(2);   // v1=2：满精力自动醒（UI 播伸懒腰 + "睡醒啦"）
                log_add((uint8_t)EventKind::WakeUp);
            }
            break;
        }
        case PetStateKind::IDLE: {
            // energy=0 → 强制睡眠（窗外也生效：时间/精力"或"关系）
            if (s_.energy <= kForceSleepEnergy) {
                enter_sleep(false);
                log_add((uint8_t)EventKind::SleepStart);
                break;
            }
            // mood≤10 → 抑郁
            if (s_.mood <= kDepressedMoodThreshold) {
                s_.pstate = PetStateKind::DEPRESSED;
                s_.state_since_pet_sec = s_.pet_seconds;
                s_.pet_count_depressed = 0;
                emit(EventKind::Depressed);
                log_add((uint8_t)EventKind::Depressed);
                break;
            }
            try_auto_sleep();
            break;
        }
        case PetStateKind::SICK:
            // 需求2：病中到点也入睡（睡醒仍回 SICK，见 SLEEPING case）
            try_auto_sleep();
            break;
        case PetStateKind::DEPRESSED:
            // 抑郁持续 1 宠物日 → 离家出走事件
            if (s_.pet_seconds - s_.state_since_pet_sec >= seconds_per_pet_day(s_.time_mode)
                && s_.active_event == SpecialEventId::None) {
                trigger_event(SpecialEventId::Runaway);
            }
            // 需求2：抑郁中到点也入睡
            try_auto_sleep();
            break;
        default:
            break;
    }
}

// 入睡统一入口：切换 SLEEPING + 关灯（补发 LightToggled 供 UI 背景切换）。
// manual=true 表示用户主动关灯入睡——不被"到点自然醒"打断。
void PetCore::enter_sleep(bool manual) {
    if (s_.pstate == PetStateKind::SLEEPING) return;
    bool was_light = s_.light_on;
    s_.pstate = PetStateKind::SLEEPING;
    s_.state_since_pet_sec = s_.pet_seconds;
    s_.sleep_manual_ = manual;
    s_.light_on = false;
    if (was_light) emit(EventKind::LightToggled, 0);
    emit(EventKind::SleepStart);
    log_add((uint8_t)EventKind::SleepStart);
}

// 醒来统一入口：恢复开灯（补发 LightToggled 供 UI 背景恢复）+ 播 WakeUp
void PetCore::exit_sleep(int wake_kind) {
    bool was_light = s_.light_on;
    s_.light_on = true;
    if (!was_light) emit(EventKind::LightToggled, 1);
    emit(EventKind::WakeUp, wake_kind);
}

// 小时是否处于睡眠窗口（支持跨午夜，如 23→6；首尾相等视为全天清醒）
bool PetCore::in_sleep_window(int hour) const {
    if (sleep_start_hour_ == sleep_wake_hour_) return false;
    if (sleep_start_hour_ < sleep_wake_hour_) {
        return hour >= sleep_start_hour_ && hour < sleep_wake_hour_;
    }
    return hour >= sleep_start_hour_ || hour < sleep_wake_hour_;
}

// 当前作息小时是否在睡眠窗口（真实模式用注入时钟小时，演示用宠物时钟）
bool PetCore::in_sleep_window_now() const {
    int hour;
    if (s_.time_mode == TimeMode::Real && real_hour_fn_) {
        hour = real_hour_fn_();
    } else {
        hour = pet_clock_from_seconds(s_.pet_seconds, s_.time_mode).hour;
    }
    return in_sleep_window(hour);
}

// 睡眠中距"精力恢复满(100)"的秒数（白天小睡深睡自动醒闹钟）。
// 恢复速率 = kRateEnergyRecover/min（演示模式 ×24 → 秒数相应缩短）。
int64_t PetCore::seconds_to_energy_full() const {
    float need = kStatMax - s_.energy;
    if (need <= 0.0f) return 0;
    const float mult = (s_.time_mode == TimeMode::Demo) ? kDemoSpeedup : 1.0f;
    float rec_per_min = kRateEnergyRecover * (s_.pstate == PetStateKind::SICK ? 0.5f : 1.0f) * mult;
    if (rec_per_min <= 0.0f) return 3600;
    int64_t min = (int64_t)(need / rec_per_min) + 1;
    int64_t sec = min * 60;
    if (sec < 30) sec = 30;
    return sec;
}

void PetCore::advance_time() {
    check_stage_evolution();   // 阶段跃迁+分支进化（等级+日龄双门槛，v5）
    check_state_transitions();
    check_death();

    // 事件超时 → 默认选项
    if (s_.active_event != SpecialEventId::None
        && s_.event_deadline_real_sec > 0
        && s_.real_seconds >= s_.event_deadline_real_sec) {
        resolve_event(event_default_choice(s_.active_event));
    }
}

// ===== 死亡 =====
void PetCore::check_death() {
    if (s_.pstate == PetStateKind::DEAD) return;
    int64_t day_sec = seconds_per_pet_day(s_.time_mode);

    // health=0 → 濒死，72 宠物小时
    if (s_.health <= 0) {
        if (s_.dying_since_pet_sec < 0) {
            s_.dying_since_pet_sec = s_.pet_seconds;
            emit(EventKind::Dying);
            log_add((uint8_t)EventKind::Dying);
        } else if (s_.pet_seconds - s_.dying_since_pet_sec
                   >= (int64_t)kDyingDeathPetHours * day_sec / 24) {
            s_.pstate = PetStateKind::DEAD;
            emit(EventKind::Died);
            log_add((uint8_t)EventKind::Died);
            return;
        }
    } else {
        s_.dying_since_pet_sec = -1;
    }

    // SICK 48 宠物小时未治
    if (s_.pstate == PetStateKind::SICK && s_.sick_since_pet_sec >= 0) {
        if (s_.pet_seconds - s_.sick_since_pet_sec
            >= (int64_t)kSickDeathPetHours * day_sec / 24) {
            s_.pstate = PetStateKind::DEAD;
            emit(EventKind::Died);
            log_add((uint8_t)EventKind::Died);
            return;
        }
    }

    // 老年每日概率
    if (s_.stage == Stage::Senior && s_.pet_seconds % day_sec < 24) {
        // 日界抽签（每宠物日一次近似）
        if (rand_pct() < kSeniorDeathChancePerDay / 24) {
            s_.pstate = PetStateKind::DEAD;
            emit(EventKind::Died);
            log_add((uint8_t)EventKind::Died);
        }
    }
}

// ===== 互动：喂食（需求 §2.1）=====
void PetCore::feed(FoodKind k) {
    if (s_.stage == Stage::Egg) { emit(EventKind::FeedRejected, 5); return; }
    if (s_.pstate == PetStateKind::DEAD) return;
    if (s_.pstate == PetStateKind::SLEEPING) { emit(EventKind::FeedRejected, 1); return; }
    if (s_.pstate == PetStateKind::DEPRESSED) { emit(EventKind::FeedRejected, 2); return; }
    if (!kFoodInfinite[(int)k] && s_.food_inv[(int)k] <= 0) { emit(EventKind::FeedRejected, 4); return; }
    if (s_.food_cooldown_pet_sec[(int)k] > s_.pet_seconds) { emit(EventKind::FeedRejected, 3); return; }
    if (k != FoodKind::Spoiled && s_.hunger >= kFeedRejectHunger) { emit(EventKind::FeedRejected, 0); return; }

    if (!kFoodInfinite[(int)k]) s_.food_inv[(int)k]--;
    const FoodDef& f = kFoods[(int)k];

    s_.hunger += f.hunger;
    s_.mood   += f.mood;
    s_.intelligence += f.int_gain;
    s_.bond   += f.bond_gain;
    // 进化分支成长联动（主食→力量 零食→速度 高级料/最爱→魔法）
    add_growth(f.grow_power, f.grow_magic, f.grow_speed);
    if (f.cooldown_pet_min > 0) {
        s_.food_cooldown_pet_sec[(int)k] =
            s_.pet_seconds + (int64_t)f.cooldown_pet_min
                              * seconds_per_pet_day(s_.time_mode) / (24 * 60);
    }

    // 零食连续 3 次 → 吃撑
    if (k == FoodKind::Snack) {
        if (++s_.snack_streak >= kSnackOvereatCount) {
            s_.snack_streak = 0;
            s_.overeat_until_pet_sec = s_.pet_seconds + 12 * seconds_per_pet_day(s_.time_mode) / 24;
            emit(EventKind::Overeat);
            log_add((uint8_t)EventKind::Overeat);
        }
    } else {
        s_.snack_streak = 0;
    }

    // 腐败食物 30% 生病（生病优先于进食动画）
    bool got_sick = false;
    if (k == FoodKind::Spoiled && rand_pct() < f.sick_chance_pct
        && s_.pstate != PetStateKind::SICK) {
        s_.pstate = PetStateKind::SICK;
        s_.state_since_pet_sec = s_.pet_seconds;
        s_.sick_since_pet_sec = s_.pet_seconds;
        got_sick = true;
        emit(EventKind::Sick);
        log_add((uint8_t)EventKind::Sick);
    } else if (k != FoodKind::Spoiled && rand_pct() < kDislikeChancePct) {
        // 随机不喜欢：mood-3（净效果）
        s_.mood -= 3;
    }

    if (!got_sick) s_.pstate = PetStateKind::EATING;
    s_.state_since_pet_sec = s_.pet_seconds;
    s_.last_interaction_pet_sec = s_.pet_seconds;
    clamp_stats();
    add_exp(kExpFeed);
    emit(EventKind::FeedOk, (int)k);
    log_add((uint8_t)EventKind::FeedOk);
}

// ===== 抚摸 =====
void PetCore::pet_touch() {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return;
    if (s_.pstate == PetStateKind::SLEEPING) {
        // 睡眠改写 v3：连点唤醒已废弃，睡觉时抚摸只"翻个身"，不醒
        emit(EventKind::PettedOk, 1);
        return;
    }
    s_.pet_count_sleeping = 0;
    s_.last_interaction_pet_sec = s_.pet_seconds;
    if (s_.pstate == PetStateKind::DEPRESSED) {
        if (++s_.pet_count_depressed >= kDepressCurePets) {
            s_.pstate = PetStateKind::IDLE;
            s_.mood += kDepressCureMood;
            s_.pet_count_depressed = 0;
            clamp_stats();
            emit(EventKind::DepressCured);
            log_add((uint8_t)EventKind::DepressCured);
        }
        emit(EventKind::PettedOk);
        return;
    }
    s_.mood += kPetMoodGain;
    s_.bond += kPetBondGain;
    clamp_stats();
    emit(EventKind::PettedOk);
}

// ===== 洗澡 =====
void PetCore::bathe() {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return;
    if (s_.pstate == PetStateKind::SLEEPING) { emit(EventKind::BatheOk, 1); return; }
    s_.hygiene = kBatheHygieneGain;
    s_.mood   += kBatheMoodGain;
    s_.poop = 0;
    s_.pstate = PetStateKind::BATHING;
    s_.state_since_pet_sec = s_.pet_seconds;
    s_.last_interaction_pet_sec = s_.pet_seconds;
    clamp_stats();
    emit(EventKind::BatheOk);
    log_add((uint8_t)EventKind::BatheOk);
}

// ===== 用药（需求 §2.4）=====
void PetCore::medicate(MedKind k) {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return;
    if (!kMedInfinite[(int)k] && s_.med_inv[(int)k] <= 0) { emit(EventKind::MedRejected, 4); return; }
    if (s_.pstate == PetStateKind::SLEEPING) { emit(EventKind::MedRejected, 1); return; }

    const MedDef& m = kMeds[(int)k];
    // 退烧药仅 SICK 可用
    if (k == MedKind::Fever && s_.pstate != PetStateKind::SICK) {
        emit(EventKind::MedRejected, 5);
        return;
    }
    // 胃药仅吃撑时有效（无吃撑也允许，只是浪费）
    if (!kMedInfinite[(int)k]) s_.med_inv[(int)k]--;

    switch (k) {
        case MedKind::Fever:
            s_.health += m.health_gain;
            s_.pstate = PetStateKind::IDLE;
            s_.sick_since_pet_sec = -1;
            emit(EventKind::Healed);
            log_add((uint8_t)EventKind::Healed);
            break;
        case MedKind::Stomach:
            s_.overeat_until_pet_sec = -1;
            s_.hunger -= 20;
            break;
        case MedKind::Vitamin:
            s_.health += m.health_gain;
            s_.immunity_until_pet_sec =
                s_.pet_seconds + (int64_t)m.immunity_pet_hours
                                  * seconds_per_pet_day(s_.time_mode) / 24;
            break;
        case MedKind::Special:
            s_.health = kStatMax;
            s_.pstate = (s_.pstate == PetStateKind::SICK) ? PetStateKind::IDLE : s_.pstate;
            s_.sick_since_pet_sec = -1;
            s_.overeat_until_pet_sec = -1;
            emit(EventKind::Healed);
            log_add((uint8_t)EventKind::Healed);
            break;
        default: break;
    }
    s_.last_interaction_pet_sec = s_.pet_seconds;
    clamp_stats();
    emit(EventKind::MedOk, (int)k);
    log_add((uint8_t)EventKind::MedOk);
}

// ===== 灯光（睡眠改写 v3 统一模型）=====
// 返回：0=操作成功 1=夜间时段无法起床（开灯无效） 2=精力<60 无法起床
int PetCore::toggle_light() {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return 0;
    if (s_.light_on) {
        // 灯亮 → 关灯入睡（手动哄睡；白天/夜间均可入睡）
        if (s_.pstate == PetStateKind::SLEEPING) return 0;
        enter_sleep(true);
        return 0;
    }
    // 灯灭 → 尝试开灯唤醒
    if (s_.pstate == PetStateKind::SLEEPING) {
        // 规则3：夜间时段（窗口内）不管精力多少都不能醒
        if (in_sleep_window_now()) return 1;
        // 规则1/2：入睡后精力未恢复到 ≥60 不允许唤醒
        if (s_.energy < kWakeMinEnergy) return 2;
        s_.pstate = (s_.sick_since_pet_sec >= 0) ? PetStateKind::SICK
                                                  : PetStateKind::IDLE;
        s_.state_since_pet_sec = s_.pet_seconds;
        s_.sleep_manual_ = false;
        s_.sleep_hint_shown = false;
        s_.auto_sleep_deadline_real = 0;
        exit_sleep(1);
        log_add((uint8_t)EventKind::WakeUp);
        return 0;
    }
    // 灯灭但没睡（异常/手动光效）：仅补回开灯
    s_.light_on = true;
    emit(EventKind::LightToggled, 1);
    return 0;
}

void PetCore::request_sleep() {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return;
    if (s_.pstate == PetStateKind::SLEEPING) return;
    enter_sleep(true);   // 内部已 emit SleepStart + log_add
}

// 睡眠改写 v3：连点唤醒机制已废弃——睡觉时抚摸只"翻个身"，起床唯一入口
// 是白天开灯（精力≥60）或白天精力睡满自动醒。

// ===== 玩耍（需求 §2.3）=====
bool PetCore::can_play(PlayKind k, int* why) {
    auto fail = [&](int w) { if (why) *why = w; return false; };
    if (s_.stage == Stage::Egg)  return fail(6);
    if (s_.pstate == PetStateKind::DEAD)   return fail(6);
    if (s_.pstate == PetStateKind::SLEEPING) return fail(1);
    if (s_.pstate == PetStateKind::DEPRESSED) return fail(2);
    if (s_.pstate == PetStateKind::SICK)   return fail(5);
    if (s_.energy < kPlayMinEnergy) return fail(3);
    if (s_.level < kPlays[(int)k].unlock_level) return fail(0);
    if (kPlays[(int)k].daily_limit > 0
        && k == PlayKind::Rhythm
        && s_.rhythm_count_today >= kPlays[(int)k].daily_limit) return fail(4);
    return true;
}

void PetCore::play_begin(PlayKind k) {
    if (!can_play(k)) return;
    s_.pstate = PetStateKind::PLAYING;
    s_.state_since_pet_sec = s_.pet_seconds;
    s_.energy -= kPlays[(int)k].energy_cost;
    s_.last_interaction_pet_sec = s_.pet_seconds;
    clamp_stats();
    emit(EventKind::PlayStart, (int)k);
}

void PetCore::play_end(PlayKind k, bool won) {
    const PlayDef& p = kPlays[(int)k];
    if (won) {
        s_.mood += p.mood_gain;
        s_.bond += p.bond_gain;
        if (p.int_gain > 0) s_.intelligence += p.int_gain;  // Perfect
        if (rand_pct() < kPlaySkillLearnChance) try_learn_skill();
    } else {
        s_.mood += p.mood_gain * 0.3f;   // 输了也有少量开心
    }
    // 进化分支成长联动（输了也运动了：按 40% 折算）
    float gf = won ? 1.0f : 0.4f;
    add_growth(p.grow_power * gf, p.grow_magic * gf, p.grow_speed * gf);
    s_.play_streak++;
    s_.rhythm_count_today += (k == PlayKind::Rhythm) ? 1 : 0;
    if (s_.pstate == PetStateKind::PLAYING) {
        s_.pstate = PetStateKind::IDLE;
        s_.state_since_pet_sec = s_.pet_seconds;
    }
    clamp_stats();
    add_exp(kExpPlay);
    emit(EventKind::PlayFinished, (int)k, won ? 1 : 0);
    log_add((uint8_t)EventKind::PlayFinished);
}

// ===== 教育（需求 §2.5）=====
bool PetCore::can_learn(EduKind k, int* why) {
    auto fail = [&](int w) { if (why) *why = w; return false; };
    if (s_.stage == Stage::Egg) return fail(6);
    if (s_.pstate == PetStateKind::DEAD) return fail(6);
    if (s_.pstate == PetStateKind::SLEEPING) return fail(1);
    if (s_.pstate == PetStateKind::DEPRESSED) return fail(2);
    if (s_.pstate == PetStateKind::SICK) return fail(5);
    if (s_.energy < kEduMinEnergy) return fail(3);
    if (s_.level < kEdus[(int)k].unlock_level) return fail(0);
    // 计数器为教学工具：不受每日教育次数上限约束
    if (k != EduKind::Counter && s_.edu_count_today >= kEduDailyLimit) return fail(4);
    return true;
}

void PetCore::edu_begin(EduKind k) {
    if (!can_learn(k)) return;
    s_.pstate = PetStateKind::LEARNING;
    s_.state_since_pet_sec = s_.pet_seconds;
    s_.energy -= kEdus[(int)k].energy_cost;
    s_.last_interaction_pet_sec = s_.pet_seconds;
    clamp_stats();
    emit(EventKind::EduStart, (int)k);
}

void PetCore::edu_end(EduKind k, int correct) {
    const EduDef& e = kEdus[(int)k];
    // 进化分支成长联动（学习→魔法，各课程 gain 见 kEdus 表）
    add_growth(0, e.grow_magic, 0);
    // 计数器：自由拨珠位值教学，固定 +1 智力；不走"X题对错"结算、不惩罚
    // 心情、不学技能；仍计入每日教育次数（与其他教育一致的限制）。
    if (k == EduKind::Counter) {
        s_.intelligence += e.int_gain_per_correct;
        // 不计入每日教育次数（计数器无次数限制）
        if (s_.pstate == PetStateKind::LEARNING) {
            s_.pstate = PetStateKind::IDLE;
            s_.state_since_pet_sec = s_.pet_seconds;
        }
        clamp_stats();
        add_exp(kExpEdu);
        emit(EventKind::EduFinished, (int)k, 1);
        log_add((uint8_t)EventKind::EduFinished);
        return;
    }
    int gain = correct * e.int_gain_per_correct;
    s_.intelligence += gain;
    s_.edu_count_today++;
    if (correct >= kEduQuestions) {
        s_.mood += 5;
        if (rand_pct() < kEduSkillLearnChance) try_learn_skill();
    } else if (correct <= 2) {
        s_.mood -= 3;
    }
    if (s_.pstate == PetStateKind::LEARNING) {
        s_.pstate = PetStateKind::IDLE;
        s_.state_since_pet_sec = s_.pet_seconds;
    }
    clamp_stats();
    add_exp(kExpEdu);
    emit(EventKind::EduFinished, (int)k, correct);
    log_add((uint8_t)EventKind::EduFinished);
}

// ===== 进化分支成长联动（喂食/玩耍/教育共同入口）=====
// 分支值 0..100：超出上限的溢出部分忽略，并按 kEvoOverflowExpRatio（5点=1经验）
// 转化为经验——成长不浪费，但刷不动上限。
void PetCore::add_growth(float power, float magic, float speed) {
    if (s_.stage == Stage::Egg || s_.pstate == PetStateKind::DEAD) return;
    float extra_exp = 0;
    auto grow = [&](float& v, float amt) {
        if (amt <= 0) return;
        float over = v + amt - kEvoStatMax;
        if (over > 0) { extra_exp += over * kEvoOverflowExpRatio; v = kEvoStatMax; }
        else           v += amt;
    };
    grow(s_.evo_power, power);
    grow(s_.evo_magic, magic);
    grow(s_.evo_speed, speed);
    if (extra_exp >= 1.0f) add_exp((int)extra_exp);
}

// 增加食物库存（商店购买/事件掉落用）。上限 99 防溢出；无限食物（主食）忽略。
void PetCore::add_food_inv(FoodKind k, int count) {
    if (k < FoodKind::Meal || k >= FoodKind::Count) return;
    if (kFoodInfinite[(int)k]) return;   // 主食无限库存，无需计数
    int v = s_.food_inv[(int)k] + (count > 0 ? count : 0);
    if (v > 99) v = 99;
    s_.food_inv[(int)k] = (uint8_t)v;
}

// ===== 技能 =====
bool PetCore::try_learn_skill() {
    for (int i = 0; i < (int)SkillId::Count; ++i) {
        if (!((s_.skills >> i) & 1)) {
            s_.skills |= (uint8_t)(1 << i);
            emit(EventKind::SkillLearned, i);
            log_add((uint8_t)EventKind::SkillLearned);
            return true;
        }
    }
    return false;   // 全部已学
}

// ===== 经验/等级 =====
void PetCore::add_exp(int amount) {
    s_.exp += amount;
    while (s_.level < kMaxLevel && s_.exp >= exp_to_next(s_.level)) {
        s_.exp -= exp_to_next(s_.level);
        s_.level++;
        // 解锁提示
        int unlock = 0;
        if (s_.level == kUnlockSnackLv)    unlock = 1;   // 零食
        if (s_.level == kUnlockWordLv)     unlock = 2;   // 认字
        if (s_.level == kUnlockMusicLv)    unlock = 4;   // 音乐
        if (s_.level == kUnlockBreedLv)    unlock = 5;   // 繁育
        emit(EventKind::LevelUp, s_.level, unlock);
        log_add((uint8_t)EventKind::LevelUp);
    }
}

// ===== 繁育（需求 §4，AI 配种）=====
bool PetCore::can_breed(int* why) {
    auto fail = [&](int w) { if (why) *why = w; return false; };
    if (s_.stage != Stage::Adult && s_.stage != Stage::Senior) return fail(1);
    if (s_.level < kBreedMinLevel) return fail(0);
    if (s_.breed_count >= kBreedMaxCount) return fail(2);
    if (s_.gestation_end_pet_sec > 0) return fail(3);
    if (s_.pstate == PetStateKind::SLEEPING || s_.pstate == PetStateKind::SICK
        || s_.pstate == PetStateKind::DEPRESSED) return fail(4);
    // 冷却：上次繁育 ≥7 宠物日（用 gestation_start 上一胎记录近似）
    if (s_.gestation_start_pet_sec > 0
        && s_.pet_seconds - s_.gestation_start_pet_sec
               < (int64_t)kBreedCooldownPetDays * seconds_per_pet_day(s_.time_mode)) {
        return fail(6);
    }
    return true;
}

void PetCore::breed_attempt() {
    if (!can_breed()) return;
    if (s_.mood < 50) { emit(EventKind::MedRejected, 7); return; }  // 没心情
    int chance = kBreedSuccessBasePct + (s_.bond >= kBreedMinBond ? 10 : 0);
    s_.last_interaction_pet_sec = s_.pet_seconds;
    if (rand_pct() >= chance) {
        emit(EventKind::MedRejected, 8);   // 相亲失败
        return;
    }
    // 孕育期 12~24 宠物小时
    int hours = kGestationMinPetHours
              + (int)(esp_random() % (kGestationMaxPetHours - kGestationMinPetHours + 1));
    s_.gestation_start_pet_sec = s_.pet_seconds;
    s_.gestation_end_pet_sec =
        s_.pet_seconds + (int64_t)hours * seconds_per_pet_day(s_.time_mode) / 24;
    s_.breed_count++;
    s_.mood -= kBreedParentMoodLoss;
    clamp_stats();
    emit(EventKind::GestationStart);
    log_add((uint8_t)EventKind::GestationStart);
}

void PetCore::finish_gestation() {
    int n = 1 + (int)(esp_random() % kBreedMaxBabies);   // 1~3
    s_.babies_total += n;
    s_.pending_eggs += n;
    s_.gestation_end_pet_sec = -1;
    // 后代继承：智力 = 本体×0.8 + 随机 0~20；技能 30% 继承 1 个
    s_.inherit_int = (int)(s_.intelligence * 0.8f) + (int)(esp_random() % 21);
    s_.inherit_skills = s_.skills;   // 简化：继承已会技能
    add_exp(kExpBreed);
    emit(EventKind::Born, n);
    log_add((uint8_t)EventKind::Born);
}

// ===== 特殊事件（需求 §5）=====
void PetCore::check_special_events() {
    // 睡眠中不触发任何随机事件（含生日/噩梦/访客/商人）。夜间深休眠补跳时
    // 整夜循环都处 SLEEPING，此短路保证深休眠期间事件数为 0（需求：夜间
    // 睡觉不打扰）；属性衰减/恢复、生病/死亡等"正常状态变化"不受影响。
    if (s_.pstate == PetStateKind::SLEEPING) return;
    // 深休眠补跳期间（可能醒着，如低电量 30 分钟自检）：随机事件同样不触发，
    // 避免"无 UI 时凭空弹事件/事件积压"。
    if (!events_enabled_) return;
    if (s_.active_event != SpecialEventId::None) return;
    // 节流：两事件至少间隔 10 宠物分钟
    int64_t throttle = seconds_per_pet_day(s_.time_mode) / (24 * 6);
    if (s_.pet_seconds - s_.last_event_pet_sec < throttle) return;

    // 生日：每 7 宠物日（街机化）自动触发
    if (s_.age_pet_days > 0 && s_.age_pet_days % 7 == 0
        && s_.last_event_pet_sec < s_.pet_seconds - seconds_per_pet_day(s_.time_mode)) {
        s_.hunger = kStatMax; s_.mood = kStatMax; s_.energy = kStatMax;
        s_.hygiene = kStatMax; s_.health = kStatMax;
        s_.food_inv[(int)FoodKind::Premium] += 1;
        s_.last_event_pet_sec = s_.pet_seconds;
        clamp_stats();
        emit(EventKind::SpecialEvent, (int)SpecialEventId::Birthday);
        emit(EventKind::GiftReceived, 0, (int)FoodKind::Premium);
        add_exp(kExpEvent);
        log_add((uint8_t)EventKind::SpecialEvent);
        return;
    }

    if (rand_pct() >= kEventChancePerTickPct) return;

    // 权重抽取
    PetClock pc = pet_clock_from_seconds(s_.pet_seconds, s_.time_mode);
    bool night = is_sleeping_hour(pc.hour) || !s_.light_on;
    struct Cand { SpecialEventId id; int w; };
    Cand pool[6] = {
        {SpecialEventId::Visitor,  night ? 0 : 30},
        {SpecialEventId::Rain,     night ? 10 : 25},
        {SpecialEventId::Nightmare,(s_.pstate == PetStateKind::SLEEPING && s_.mood < 40) ? 40 : 0},
        {SpecialEventId::Meteor,   (s_.age_pet_days == 7 || s_.age_pet_days == 14
                                    || s_.age_pet_days == 21) && night ? 50 : 0},
        {SpecialEventId::Merchant, night ? 20 : 0},
        {SpecialEventId::Runaway,  0},   // 只由抑郁超时触发
    };
    int total = 0;
    for (auto& c : pool) total += c.w;
    if (total <= 0) return;
    int roll = (int)(esp_random() % total);
    for (auto& c : pool) {
        if (c.w <= 0) continue;
        if (roll < c.w) { trigger_event(c.id); return; }
        roll -= c.w;
    }
}

void PetCore::trigger_event(SpecialEventId id) {
    s_.active_event = id;
    s_.last_event_pet_sec = s_.pet_seconds;
    s_.event_deadline_real_sec = s_.real_seconds + kEventPopupTimeoutSec;
    emit(EventKind::SpecialEvent, (int)id);
    log_add((uint8_t)EventKind::SpecialEvent);
}

void PetCore::resolve_event(int choice) {
    if (s_.active_event == SpecialEventId::None) return;
    SpecialEventId id = s_.active_event;
    s_.active_event = SpecialEventId::None;
    s_.event_deadline_real_sec = 0;

    // 稀有礼物三选一：高级料 / 最爱 / 特效药（访客+神秘商人共用掉落池）
    auto give_rare = [this]() {
        int r = (int)(esp_random() % 3);
        if (r == 0)      { s_.food_inv[(int)FoodKind::Premium]  += 1; emit(EventKind::GiftReceived, 0, (int)FoodKind::Premium); }
        else if (r == 1) { s_.food_inv[(int)FoodKind::Favorite] += 1; emit(EventKind::GiftReceived, 0, (int)FoodKind::Favorite); }
        else             { s_.med_inv[(int)MedKind::Special]    += 1; emit(EventKind::GiftReceived, 1, (int)MedKind::Special); }
    };

    switch (id) {
        case SpecialEventId::Visitor:
            if (choice == 0) {   // 欢迎
                s_.mood += 10;
                give_rare();
                add_exp(kExpEvent);
            }
            break;
        case SpecialEventId::Rain:
            if (choice == 0) {   // 打伞出去
                s_.hygiene -= 20;
                s_.mood += 15;
            }
            break;
        case SpecialEventId::Nightmare:
            if (choice == 0) {   // 安慰
                s_.mood += 15;
                // 唤醒
                if (s_.pstate == PetStateKind::SLEEPING) {
                    s_.pstate = PetStateKind::IDLE;
                    s_.light_on = true;
                    emit(EventKind::WakeUp, 0);
                }
            } else {
                s_.mood -= 10;
            }
            break;
        case SpecialEventId::Meteor:
            if (choice == 0) {   // 许愿
                s_.food_inv[(int)FoodKind::Premium] += 1;
                emit(EventKind::GiftReceived, 0, (int)FoodKind::Premium);
                add_exp(kExpEvent);
            }
            break;
        case SpecialEventId::Merchant:
            if (choice == 0) {   // 交易
                if (rand_pct() < 90) {   // 10% 被骗
                    give_rare();
                } else {
                    s_.mood -= 10;
                }
            }
            break;
        case SpecialEventId::Runaway:
            if (choice == 0) {   // 追回
                s_.mood = 30;
                s_.pstate = PetStateKind::IDLE;
                emit(EventKind::DepressCured);
            } else {             // 没追回：bond-50，mood 恢复 30
                s_.bond -= 50;
                s_.mood = 30;
                s_.pstate = PetStateKind::IDLE;
            }
            break;
        default:
            break;
    }
    clamp_stats();
    emit(EventKind::EventResolved, (int)id, choice);
    log_add((uint8_t)EventKind::EventResolved);
}

// ===== 重置（死亡后）=====
void PetCore::reset_to_new_egg() {
    bool has_egg = s_.pending_eggs > 0;
    int  gen = s_.generation + (has_egg ? 1 : 0);
    int  inherit_int = has_egg ? s_.inherit_int : 0;
    uint8_t inherit_skills = has_egg ? s_.inherit_skills : 0;
    int pending = has_egg ? s_.pending_eggs - 1 : 0;

    PetState fresh{};
    fresh.time_mode = s_.time_mode;
    fresh.generation = gen;
    fresh.pending_eggs = pending;
    // 继承：智力 + 天赋技能（Lv3 自动解锁 → 直接给）
    fresh.intelligence = 10 + inherit_int;
    if (has_egg) {
        // 30% 概率各技能（简化：全部继承的技能）
        fresh.skills = inherit_skills;
    }
    s_ = fresh;
    // 蛋皮随机 1/4（EvoLook::Egg0..Egg3），新蛋外观不重样
    s_.evo_look = (uint8_t)(esp_random() % 4);
    tick_accum_sec_ = 0;
    emit(EventKind::StageChanged, (int)Stage::Egg);
}

// ===== 注意图标 =====
void PetCore::apply_attention() {
    int bits = 0;
    if (s_.hunger  < 20) bits |= (1 << 0);
    if (s_.mood    < 20) bits |= (1 << 1);
    if (s_.pstate == PetStateKind::SICK) bits |= (1 << 2);
    if (s_.pstate == PetStateKind::SLEEPING) bits |= (1 << 3);
    if (s_.hygiene < 20 || s_.poop >= 3) bits |= (1 << 4);
    if (s_.pstate == PetStateKind::DEPRESSED) bits |= (1 << 5);
    if (s_.dying_since_pet_sec >= 0) bits |= (1 << 6);
    if (bits != last_attn_bits_) {
        last_attn_bits_ = bits;
        emit(EventKind::AttentionFlash, bits);
    }
}

}  // namespace boxpet::game
