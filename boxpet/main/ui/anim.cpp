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

static inline const sprites::Sprite* by_name_or_null(const sprites::Sprite* arr, int n, const char* name) {
    for (int i = 0; i < n; ++i) {
        if (std::strcmp(arr[i].name, name) == 0) return &arr[i];
    }
    return nullptr;
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

    // 持久状态帧（优先于阶段 idle 帧）
    switch (st.pstate) {
        case game::PetStateKind::SLEEPING: {
            // 睡觉帧按当前阶段基色参数化生成（zzz_baby/zzz_child/.../zzz_senior）。
            // 旧代码写死查 ksenior 表的裸名 "zzz"——该名字不存在（实际是
            // zzz_senior），返回空帧 → 画布保留醒着时的最后一帧，
            // 表现为"关灯睡觉时宠物样子和醒着一样"（少年阶段最明显）。
            const char* sk = action_stage_key(st);
            char name[32];
            snprintf(name, sizeof(name), "zzz_%s", sk);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return by_name_or_null(ksenior_frames, ksenior_count, "zzz_senior");
        }
        case game::PetStateKind::SICK:
            return find_stage_sprite("sick", st);
        case game::PetStateKind::DEPRESSED:
            return find_stage_sprite("scold", st);
        default:
            break;
    }

    switch (st.stage) {
        case game::Stage::Egg:     return by_name_or_null(kegg_frames, kegg_count, "egg");
        case game::Stage::Baby:    return by_name_or_null(kbaby_frames, kbaby_count, "baby");
        case game::Stage::Juvenile:return by_name_or_null(kchild_frames, kchild_count, "child");
        case game::Stage::Adult:
            // 进化分支（需求 §3.2）：映射到现有三套成体精灵
            switch (st.evo_form) {
                case game::EvoForm::Scholar:  return by_name_or_null(kadult_tuan_frames, kadult_tuan_count, "adult_tuan");
                case game::EvoForm::Active:   return by_name_or_null(kadult_tang_frames, kadult_tang_count, "adult_tang");
                case game::EvoForm::Graceful: return by_name_or_null(kadult_tang_frames, kadult_tang_count, "adult_tang");
                case game::EvoForm::Radiant:  return by_name_or_null(kadult_star_frames, kadult_star_count, "adult_star");
                default:                      return by_name_or_null(kadult_star_frames, kadult_star_count, "adult_star");
            }
        case game::Stage::Senior:  return by_name_or_null(ksenior_frames, ksenior_count, "senior");
        case game::Stage::Dead:    break;
    }
    return by_name_or_null(ksenior_frames, ksenior_count, "dead_grave");
}

// 把 stage + evo_form 映射到 sprite 表里 happy_xxx / eat_xxx / zzz_xxx 后缀
//（动画帧按当前阶段基色参数化生成，由 sprite_gen2.py 输出 happy_baby/child/.../senior 等表）
static const char* action_stage_key(const game::PetState& st) {
    if (st.stage == game::Stage::Egg) return "baby";  // 蛋期无对应，兜底用 baby
    if (st.stage == game::Stage::Baby) return "baby";
    if (st.stage == game::Stage::Juvenile) return "child";
    if (st.age_pet_days >= 4 && st.age_pet_days < 11) return "teen";
    if (st.stage == game::Stage::Adult) {
        switch (st.evo_form) {
            case game::EvoForm::Scholar:  return "adult_tuan";
            case game::EvoForm::Active:   return "adult_tang";
            case game::EvoForm::Graceful: return "adult_tang";
            case game::EvoForm::Radiant:  return "adult_star";
            default:                      return "adult_star";
        }
    }
    if (st.stage == game::Stage::Senior) return "senior";
    return "baby";
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

const sprites::Sprite* SpriteAnimator::action_frame() {
    if (!pet_) return nullptr;
    const char* sk = action_stage_key(pet_->state());
    char name[32];
    switch (action_) {
        case AnimAction::Feed: {
            // 优先按 stage 选 eat_xxx；找不到再回退到通用 eat
            snprintf(name, sizeof(name), "eat_%s", sk);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return by_name_or_null(ksenior_frames, ksenior_count, "eat");
        }
        case AnimAction::Sick:    return find_stage_sprite("sick", pet_->state());
        case AnimAction::Scold:   return find_stage_sprite("scold", pet_->state());
        case AnimAction::Happy: {
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
        case AnimAction::Bath: {
            // Bath 用 happy 作为模板（享受搓澡），按 stage 选基色版
            snprintf(name, sizeof(name), "happy_%s", sk);
            const sprites::Sprite* f = find_sprite_by_name(name);
            if (f) return f;
            return by_name_or_null(ksenior_frames, ksenior_count, "happy");
        }
        default:                  return nullptr;
    }
}

// 全局：跨 sprite 表按名取帧（给非 animator 的 UI 用）
const sprites::Sprite* find_sprite_by_name(const char* name) {
    if (!name) return nullptr;
    const sprites::Sprite* tables[] = {
        kegg_frames, kbaby_frames, kchild_frames, kteen_frames,
        kadult_star_frames, kadult_tuan_frames, kadult_tang_frames,
        ksenior_frames,
    };
    const int counts[] = {
        kegg_count, kbaby_count, kchild_count, kteen_count,
        kadult_star_count, kadult_tuan_count, kadult_tang_count,
        ksenior_count,
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
            case AnimAction::Med:    // 苦得摇头：左右晃
                x_off_ = ((now_ms / 160) % 2) ? 2 : -2;
                break;
            case AnimAction::Bath:   // 搓澡：大幅上下弹跳
                y_off_ = ((now_ms / 250) % 2) ? -3 : 1;
                break;
            case AnimAction::Happy:  // 开心：轻快点头
                y_off_ = ((now_ms / 200) % 2) ? -2 : 0;
                break;
            default:                 // 其他动作：通用浮动
                y_off_ = ((now_ms / 200) % 4) - 2;
                break;
        }
        const sprites::Sprite* f = action_frame();
        if (out_changed) *out_changed = (f != last_frame_);
        last_frame_ = f;
        return f;
    }
    x_off_ = 0; y_off_ = 0;
    const sprites::Sprite* f = select_idle_frame(now_ms);
    // 呼吸动画：每 600ms 上下 1px（渲染 2x = 屏幕 2px）
    if (f && std::strcmp(f->name, "egg") != 0) {
        y_off_ = ((now_ms / 600) % 2) ? -1 : 0;
    }
    if (out_changed) *out_changed = (f != last_frame_) || (y_off_ != last_y_off_);
    last_y_off_ = y_off_;
    last_frame_ = f;
    return f;
}

}  // namespace boxpet::ui