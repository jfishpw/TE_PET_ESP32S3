// anim.cpp — 像素精灵帧调度
// 规则：
//   * 当前动作 != None 且未到期 → 返回动作帧
//   * 否则 → 按 Stage 选 idle 帧，0.5s 切换相位以产生呼吸
#include "anim.h"
#include "game/pet.h"
#include "game/pet_event.h"

#include <cstdio>
#include <cstring>

namespace boxpet::ui {

using namespace ::boxpet::sprites;

void SpriteAnimator::attach(game::PetCore* pet) {
    pet_ = pet;
}

void SpriteAnimator::trigger(AnimAction a, int duration_ms) {
    // 死亡为持久动作（直到玩家重置）
    if (a == AnimAction::Died) {
        action_ = a;
        action_until_ms_ = INT64_MAX;
        return;
    }
    action_ = a;
    // 第一次 tick 会把 action_pending_until_ms 转成绝对时间
    action_pending_until_ms_ = duration_ms;
    action_until_ms_ = 0;
}

// 进化演出：记下进化前帧，动作期内与"新形态 idle 帧"交替闪烁。
// 期间忽略与演出冲突的普通动作触发（演出结束前 action_=Evolve 优先）。
void SpriteAnimator::trigger_evolve(const sprites::Sprite* from, int duration_ms) {
    evolve_from_ = from;
    action_ = AnimAction::Evolve;
    action_pending_until_ms_ = duration_ms;
    action_until_ms_ = 0;
}

static inline const sprites::Sprite* by_name_or_null(const sprites::Sprite* arr, int n, const char* name) {
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(arr[i].name, name) == 0) return &arr[i];
    }
    return nullptr;
}

// ===== 外观资源映射表（EvoLook → 帧表 + idle 帧名 + 动作帧后缀 key）=====
// v5：全部形态帧集中在 kform_frames（蛋皮/幼生/成长/成熟/完全体 ×力魔速），
// 老年沿用 ksenior_frames。动作帧按 "<base>_<key>" 参数化命名（如
// eat_growth_magic），切换分支/阶段无需缩放适配（渲染层统一 2x）。
struct LookDef {
    const sprites::Sprite* frames;   // idle 帧所在表
    int                    count;
    const char*            idle;     // idle 帧名（=形态名）
    const char*            key;      // 动作帧后缀（happy_/eat_/zzz_/sick_/scold_）
};
static const LookDef kEvoLooks[(int)game::EvoLook::Count] = {
    /* Egg0..3 */ {kform_frames,   kform_count,   "egg0",            "baby"},  // 蛋无动作帧
    /* Egg1    */ {kform_frames,   kform_count,   "egg1",            "baby"},
    /* Egg2    */ {kform_frames,   kform_count,   "egg2",            "baby"},
    /* Egg3    */ {kform_frames,   kform_count,   "egg3",            "baby"},
    /* BabyForce    */ {kform_frames, kform_count, "baby_force",     "baby_force"},
    /* BabyMagic    */ {kform_frames, kform_count, "baby_magic",     "baby_magic"},
    /* BabySpeed    */ {kform_frames, kform_count, "baby_speed",     "baby_speed"},
    /* GrowthForce  */ {kform_frames, kform_count, "growth_force",   "growth_force"},
    /* GrowthMagic  */ {kform_frames, kform_count, "growth_magic",   "growth_magic"},
    /* GrowthSpeed  */ {kform_frames, kform_count, "growth_speed",   "growth_speed"},
    /* MatureForce  */ {kform_frames, kform_count, "mature_force",   "mature_force"},
    /* MatureMagic  */ {kform_frames, kform_count, "mature_magic",   "mature_magic"},
    /* MatureSpeed  */ {kform_frames, kform_count, "mature_speed",   "mature_speed"},
    /* UltimateForce*/ {kform_frames, kform_count, "ultimate_force", "ultimate_force"},
    /* UltimateMagic*/ {kform_frames, kform_count, "ultimate_magic", "ultimate_magic"},
    /* UltimateSpeed*/ {kform_frames, kform_count, "ultimate_speed", "ultimate_speed"},
    /* Senior  */ {ksenior_frames, ksenior_count, "senior",          "senior"},
};
static const LookDef& look_def(uint8_t look_id) {
    int i = (int)look_id;
    if (i < 0 || i >= (int)game::EvoLook::Count) i = (int)game::EvoLook::Senior;
    return kEvoLooks[i];
}

// 按 外观资源ID 取 idle 帧（导出：进化动画"旧形态"帧用）
const sprites::Sprite* look_idle_sprite(uint8_t look_id) {
    const LookDef& ld = look_def(look_id);
    return by_name_or_null(ld.frames, ld.count, ld.idle);
}

// 前置声明（定义在下方，select_idle_frame 需先调用）
static const char* action_stage_key(const game::PetState& st);
const sprites::Sprite* find_stage_sprite(const char* base, const game::PetState& st);

const sprites::Sprite* SpriteAnimator::select_idle_frame(int64_t now_ms) {
    (void)now_ms;
    if (!pet_) return nullptr;
    return idle_frame_for(pet_->state());
}

// 阶段/状态 idle 帧（自由函数，导出给聊天等场景复用）
const sprites::Sprite* idle_frame_for(const game::PetState& st) {
    const uint8_t look = st.evo_look;
    const LookDef& ld = look_def(look);

    // 死亡：墓碑帧（animator 主路径已处理，此处兜底给直接调用方）
    if (st.pstate == game::PetStateKind::DEAD || st.stage == game::Stage::Dead) {
        return by_name_or_null(ksenior_frames, ksenior_count, "dead_grave");
    }

    // 蛋期：蛋皮 idle（无属性外观，不参与低状态表情）
    if (st.stage == game::Stage::Egg) {
        const sprites::Sprite* f = by_name_or_null(ld.frames, ld.count, ld.idle);
        if (f) return f;
        return by_name_or_null(kform_frames, kform_count, "egg0");
    }

    // 持久状态帧（优先于阶段 idle 帧）：按外观 key 取参数化帧
    switch (st.pstate) {
        case game::PetStateKind::SLEEPING: {
            // 睡觉帧 zzz_<key>（各形态专属，找不到回退通用 zzz）
            char name[40];
            snprintf(name, sizeof(name), "zzz_%s", ld.key);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return find_sprite_by_name("zzz");
        }
        case game::PetStateKind::SICK:
            return find_stage_sprite("sick", st);
        case game::PetStateKind::DEPRESSED:
            return find_stage_sprite("scold", st);
        default:
            break;
    }

    // ===== 属性联动 idle 外观（需求：精力/卫生/饥饿/心情 低于60 → 外观变化）=====
    // 仅醒着 IDLE 时生效（睡眠/生病/抑郁有专属帧）。四种低状态另有四角
    // 图标（status_icons）同屏叠加，表情帧只负责"难受"的肢体语言：
    // 优先级：疲惫/低落/饥饿（scold 委屈垂头）> 卫生（sick 蔫蔫样）。
    if (st.pstate == game::PetStateKind::IDLE) {
        bool low_mood    = st.mood    <  game::kLowMoodIdleThreshold;
        bool low_energy  = st.energy  <  game::kIdleTiredEnergy;
        bool low_hunger  = st.hunger  <  game::kLowHungerIdleThreshold;
        bool low_hygiene = st.hygiene <  game::kLowHygieneIdleThreshold;
        if (low_mood || low_energy || low_hunger) return find_stage_sprite("scold", st);
        if (low_hygiene)                          return find_stage_sprite("sick", st);
    }

    // 正常 idle：外观映射表的形态帧
    return by_name_or_null(ld.frames, ld.count, ld.idle);
}

// 外观ID → 动作帧后缀 key（happy_xxx / eat_xxx / zzz_xxx / sick_xxx / scold_xxx）
static const char* action_stage_key(const game::PetState& st) {
    return look_def(st.evo_look).key;
}

// 跨表按 stage 选参数化动作帧：先查 "<base>_<stage>"，找不到回退裸 "<base>"。
// 导出给 ui_game 等非 animator 场景复用（需求3：失败反馈帧颜色随阶段一致）。
const sprites::Sprite* find_stage_sprite(const char* base, const game::PetState& st) {
    char name[32];
    snprintf(name, sizeof(name), "%s_%s", base, action_stage_key(st));
    const sprites::Sprite* f = find_sprite_by_name(name);
    if (f) return f;
    return find_sprite_by_name(base);
}

const sprites::Sprite* SpriteAnimator::action_frame(int64_t now_ms) {
    if (!pet_) return nullptr;
    const char* sk = action_stage_key(pet_->state());
    char name[32];
    switch (action_) {
        case AnimAction::Evolve: {
            // 进化演出：新形态 ↔ 旧形态 每 300ms 交替（配合白色光效覆盖层）。
            // 新形态 = 当前 idle 帧（evo_look 已是进化后外观）。
            const sprites::Sprite* now_f = idle_frame_for(pet_->state());
            bool phase = ((now_ms / 300) % 2) != 0;
            return phase ? now_f : (evolve_from_ ? evolve_from_ : now_f);
        }
        case AnimAction::Feed: {
            // 优先按 stage 选 eat_xxx；找不到再回退到通用 eat
            snprintf(name, sizeof(name), "eat_%s", sk);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return by_name_or_null(ksenior_frames, ksenior_count, "eat");
        }
        case AnimAction::Sick:    return find_stage_sprite("sick", pet_->state());
        case AnimAction::Scold:   return find_stage_sprite("scold", pet_->state());
        case AnimAction::Happy:
        case AnimAction::Pat:
        case AnimAction::Bath: {
            // 开心/撒娇/洗澡：同一组开心帧，仅运动模式不同（见 tick）
            snprintf(name, sizeof(name), "happy_%s", sk);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return by_name_or_null(ksenior_frames, ksenior_count, "happy");
        }
        case AnimAction::Died:    return by_name_or_null(ksenior_frames, ksenior_count, "dead_grave");
        case AnimAction::Sleep: {
            snprintf(name, sizeof(name), "zzz_%s", sk);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return by_name_or_null(ksenior_frames, ksenior_count, "zzz");
        }
        case AnimAction::Wedding: return by_name_or_null(ksenior_frames, ksenior_count, "wedding");
        case AnimAction::Born:    return by_name_or_null(ksenior_frames, ksenior_count, "born");
        case AnimAction::Med:     return by_name_or_null(ksenior_frames, ksenior_count, "scold");   // 苦脸
        default:                  return nullptr;
    }
}

// 全局：跨 sprite 表按名取帧（给非 animator 的 UI 用）
const sprites::Sprite* find_sprite_by_name(const char* name) {
    if (!name) return nullptr;
    const sprites::Sprite* tables[] = {
        kform_frames, ksenior_frames,
    };
    const int counts[] = {
        kform_count, ksenior_count,
    };
    for (size_t i = 0; i < sizeof(tables)/sizeof(tables[0]); ++i) {
        const sprites::Sprite* f = by_name_or_null(tables[i], counts[i], name);
        if (f) return f;
    }
    return nullptr;
}

const sprites::Sprite* SpriteAnimator::tick(int64_t now_ms, bool* out_changed) {
    // 处理触发延迟转绝对
    if (action_pending_until_ms_ > 0 && action_until_ms_ == 0) {
        action_until_ms_ = now_ms + action_pending_until_ms_;
        action_pending_until_ms_ = 0;
    }
    // 死亡时强制显示墓碑（新模型：pstate=DEAD）
    if (pet_ && (pet_->state().pstate == game::PetStateKind::DEAD
                 || pet_->state().stage == game::Stage::Dead)) {
        const sprites::Sprite* f = by_name_or_null(ksenior_frames, ksenior_count, "dead_grave");
        if (out_changed) *out_changed = (f != last_frame_);
        last_frame_ = f;
        x_off_ = 0; y_off_ = 0;
        return f;
    }
    if (action_ != AnimAction::None && now_ms >= action_until_ms_) {
        action_ = AnimAction::None;
    }
    if (action_ != AnimAction::None) {
        // 分动作运动模式（渲染 ×2 后为屏幕像素）
        x_off_ = 0;
        y_off_ = 0;
        switch (action_) {
            case AnimAction::Feed:   // 咀嚼：快速小幅上下
                y_off_ = ((now_ms / 150) % 2) ? -1 : 0;
                break;
            case AnimAction::Evolve: // 进化：原地发光感（轻微持续浮动）
                y_off_ = ((now_ms / 250) % 2) ? -1 : 0;
                break;
            case AnimAction::Med:    // 苦得摇头：左右晃
                x_off_ = ((now_ms / 160) % 2) ? 2 : -2;
                break;
            case AnimAction::Bath:   // 搓澡：大幅上下弹跳
                y_off_ = ((now_ms / 250) % 2) ? -3 : 1;
                break;
            case AnimAction::Pat:    // 抚摸撒娇：左右蹭（配开心脸）
                x_off_ = ((now_ms / 120) % 2) ? 2 : -2;
                break;
            case AnimAction::Happy:  // 开心：轻快点头
                y_off_ = ((now_ms / 200) % 2) ? -2 : 0;
                break;
            default:                 // 其他动作：通用浮动
                y_off_ = ((now_ms / 200) % 4) - 2;
                break;
        }
        const sprites::Sprite* f = action_frame(now_ms);
        if (out_changed) *out_changed = (f != last_frame_);
        last_frame_ = f;
        return f;
    }
    x_off_ = 0; y_off_ = 0;
    const sprites::Sprite* f = select_idle_frame(now_ms);
    // 呼吸动画：每 600ms 上下 1px（渲染 2x = 屏幕 2px）；蛋皮不呼吸
    if (f && std::strncmp(f->name, "egg", 3) != 0) {
        y_off_ = ((now_ms / 600) % 2) ? -1 : 0;
    }
    if (out_changed) *out_changed = (f != last_frame_) || (y_off_ != last_y_off_);
    last_y_off_ = y_off_;
    last_frame_ = f;
    return f;
}

}  // namespace boxpet::ui