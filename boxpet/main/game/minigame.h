// minigame.h — 「左右猜」小游戏（复刻初代拓麻歌子）
#pragma once

#include <cstdint>

namespace boxpet::game {

class GuessDirection {
public:
    enum class Phase : uint8_t {
        Idle = 0,
        Thinking,   // 宠物"思考中"
        Choosing,   // 等待玩家用左/右+中键猜方向
        Reveal,     // 显示结果
        Finished,   // 5 回合完成
    };

    struct Result {
        int   rounds_won;
        int   rounds_total;
        bool  won;
    };

    GuessDirection();
    void reset();
    void start_new_round();
    void choose_left();
    void choose_right();
    void confirm();          // 中键提交
    void tick(int64_t now_ms); // 由外部按时间调用（动画帧切换用）

    Phase phase() const { return phase_; }
    int   current_round() const { return round_; }   // 1..5
    int   rounds_won() const { return wins_; }
    bool  last_was_left() const { return last_dir_left_; }
    bool  player_picked_left() const { return player_picked_left_; }

    // 完成时调用 start_new_game() 之前由 caller 决定
    Result finish() const;  // 计算 won 字段并返回

    // 玩家选错还是选对（最近一回合）
    bool last_correct() const { return last_correct_; }

private:
    Phase phase_ = Phase::Idle;
    int   round_ = 0;
    int   wins_  = 0;
    bool  last_dir_left_ = false;     // 宠物这一回合想的方向
    bool  player_picked_left_ = false;
    bool  player_submitted_ = false;   // 是否已按中键提交
    bool  last_correct_ = false;
    int64_t phase_until_ms_ = 0;
    int   reveal_frame_ = 0;
    void  enter(Phase p, int duration_ms, int64_t now_ms);
};

}  // namespace boxpet::game