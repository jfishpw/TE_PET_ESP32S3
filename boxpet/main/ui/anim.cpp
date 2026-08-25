// anim.cpp — 像素精灵帧调度
// 规则：
//   * 当前动作 != None 且未到期 → 返回动作帧
//   * 否则 → 按 Stage 选 idle 帧，0.5s 切换相位以产生呼吸
#include "anim.h"
#include "game/pet.h"
#include "game/pet_event.h"

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

const sprites::Sprite* SpriteAnimator::select_idle_frame(int64_t now_ms) {
    (void)now_ms;
    if (!pet_) return nullptr;
    const game::PetState& st = pet_->state();

    // 持久状态帧（优先于阶段 idle 帧）
    switch (st.pstate) {
        case game::PetStateKind::SLEEPING:
            return by_name_or_null(ksenior_frames, ksenior_count, "zzz");
        case game::PetStateKind::SICK:
            return by_name_or_null(ksenior_frames, ksenior_count, "sick");
        case game::PetStateKind::DEPRESSED:
            return by_name_or_null(ksenior_frames, ksenior_count, "scold");
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

const sprites::Sprite* SpriteAnimator::action_frame() {
    switch (action_) {
        case AnimAction::Feed:    return by_name_or_null(ksenior_frames, ksenior_count, "eat");
        case AnimAction::Sick:    return by_name_or_null(ksenior_frames, ksenior_count, "sick");
        case AnimAction::Scold:   return by_name_or_null(ksenior_frames, ksenior_count, "scold");
        case AnimAction::Happy:   return by_name_or_null(ksenior_frames, ksenior_count, "happy");
        case AnimAction::Died:    return by_name_or_null(ksenior_frames, ksenior_count, "dead_grave");
        case AnimAction::Sleep:   return by_name_or_null(ksenior_frames, ksenior_count, "zzz");
        case AnimAction::Wedding: return by_name_or_null(ksenior_frames, ksenior_count, "wedding");
        case AnimAction::Born:    return by_name_or_null(ksenior_frames, ksenior_count, "born");
        case AnimAction::Med:     return by_name_or_null(ksenior_frames, ksenior_count, "scold");   // 苦脸
        case AnimAction::Bath:    return by_name_or_null(ksenior_frames, ksenior_count, "happy");   // 洗得开心
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