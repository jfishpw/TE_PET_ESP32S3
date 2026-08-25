// minigame.cpp — 「左右猜」实现
// 流程：
//   Idle → (start) Thinking (1s) → Choosing (5s, 等玩家按左/右后中键) → Reveal (1s)
//   5 回合后 Finished，调用 finish() 返回结果（won = rounds_won >= 3）
#include "minigame.h"
#include <esp_random.h>
#include <esp_timer.h>
#include <cstdlib>

namespace boxpet::game {

GuessDirection::GuessDirection() { reset(); }

void GuessDirection::reset() {
    phase_   = Phase::Idle;
    round_   = 0;
    wins_    = 0;
    last_dir_left_ = false;
    player_picked_left_ = false;
    player_submitted_ = false;
    last_correct_ = false;
    phase_until_ms_ = 0;
    reveal_frame_ = 0;
}

void GuessDirection::enter(Phase p, int duration_ms, int64_t now_ms) {
    phase_ = p;
    phase_until_ms_ = now_ms + duration_ms;
}

void GuessDirection::start_new_round() {
    if (phase_ == Phase::Idle || phase_ == Phase::Finished) {
        round_ = 0;
        wins_ = 0;
    }
    if (round_ >= 5) {
        enter(Phase::Finished, 0, 0);
        return;
    }
    round_++;
    last_dir_left_ = (esp_random() & 1) != 0;
    player_picked_left_ = false;
    player_submitted_ = false;
    last_correct_ = false;
    enter(Phase::Thinking, 1000, esp_timer_get_time() / 1000);
    // 注：实际 now_ms 由外部 tick 提供；这里偷懒用 esp_timer
}

void GuessDirection::choose_left()  { player_picked_left_ = true; }
void GuessDirection::choose_right() { player_picked_left_ = false; }

void GuessDirection::confirm() {
    if (phase_ != Phase::Choosing) return;
    player_submitted_ = true;
    last_correct_ = (player_picked_left_ == last_dir_left_);
    if (last_correct_) wins_++;
    reveal_frame_ = 0;
    enter(Phase::Reveal, 1500, esp_timer_get_time() / 1000);
}

void GuessDirection::tick(int64_t now_ms) {
    if (phase_ == Phase::Idle || phase_ == Phase::Finished) return;
    if (now_ms < phase_until_ms_) return;
    switch (phase_) {
        case Phase::Thinking:
            enter(Phase::Choosing, 8000, now_ms);  // 8 秒等待玩家输入
            break;
        case Phase::Choosing:
            // 超时未提交 → 算输
            last_correct_ = false;
            enter(Phase::Reveal, 1500, now_ms);
            break;
        case Phase::Reveal:
            // 自动进入下一回合
            if (round_ >= 5) {
                enter(Phase::Finished, 0, now_ms);
            } else {
                start_new_round();
            }
            break;
        default:
            break;
    }
}

GuessDirection::Result GuessDirection::finish() const {
    Result r;
    r.rounds_total = 5;
    r.rounds_won = wins_;
    r.won = wins_ >= 3;
    return r;
}

}  // namespace boxpet::game