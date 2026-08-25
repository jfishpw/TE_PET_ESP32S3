// anim.h — 像素精灵帧调度器（按阶段 + 当前动作状态选帧）
#pragma once

#include <cstdint>
#include "game/pet.h"
#include "sprites/sprites.h"

namespace boxpet::ui {

// 事件动作优先级（高优先级覆盖 idle）
enum class AnimAction : uint8_t {
    None      = 0,
    Feed      = 1,   // 吃饭动画：咀嚼小幅上下，~1.5s
    Sick      = 2,   // 生病动画
    Scold     = 3,   // 委屈动画
    Happy     = 4,   // 开心动画：轻快点头
    Died      = 5,   // 死亡动画（墓碑/天使）
    Sleep     = 6,   // 睡觉（Zzz）
    Wedding   = 7,   // 结婚（双宠爱心帧）
    Born      = 8,   // 宝宝出生（破壳探头帧）
    Med       = 9,   // 吃药：皱眉左右摇头（苦），~1.2s
    Bath      = 10,  // 洗澡：大幅上下弹跳（搓澡），~1.6s
};

class SpriteAnimator {
public:
    void attach(game::PetCore* pet);

    void trigger(AnimAction a, int duration_ms = 1000);

    // 建议 30~60Hz 调用；返回当前应显示的 sprite。
    const sprites::Sprite* tick(int64_t now_ms, bool* out_changed = nullptr);

    // 当前动作（None = idle，可供空闲行为系统判断）
    AnimAction current_action() const { return action_; }

    // 当前帧的偏移（用于 Zzz 漂浮）
    int x_offset() const { return x_off_; }
    int y_offset() const { return y_off_; }

private:
    game::PetCore* pet_ = nullptr;
    AnimAction action_  = AnimAction::None;
    int64_t action_until_ms_ = 0;        // 动作到期绝对时间（ms）
    int64_t action_pending_until_ms_ = 0;// 待转绝对的相对超时
    int x_off_ = 0, y_off_ = 0;
    int last_y_off_ = 0;
    const sprites::Sprite* last_frame_ = nullptr;

    const sprites::Sprite* select_idle_frame(int64_t now_ms);
    const sprites::Sprite* action_frame();
};

// 全局：跨所有 sprite 表按名字查找帧（游戏 / 死亡画面等 UI 用）
const sprites::Sprite* find_sprite_by_name(const char* name);

}  // namespace boxpet::ui