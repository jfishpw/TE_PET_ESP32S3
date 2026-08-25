// bsp/audio.h — ES8311 + I2S 音效合成器（正弦波，无破音）
#pragma once

#include <cstdint>
#include "esp_err.h"

namespace boxpet::bsp {

enum class Sound : uint8_t {
    Beep        = 0,   // 按键确认
    Call        = 1,   // 撒娇/异常提醒（叮咚）
    Feed        = 2,   // 进食（咀嚼）
    Reject      = 3,   // 拒绝
    Heal        = 4,   // 治愈
    Flush       = 5,   // 冲水（白噪声）
    Win         = 6,   // 胜利
    Lose        = 7,   // 失败
    Evolve      = 8,   // 进化
    Hatch       = 9,   // 孵化
    Die         = 10,  // 死亡
    Tick        = 11,  // 切换图标（轻 tick）
    Correct     = 12,  // 游戏答对
    Wrong       = 13,  // 游戏答错
    Sleep       = 14,  // 关灯晚安
};

esp_err_t audio_init();
void      audio_set_muted(bool muted);
bool      audio_is_muted();
void      audio_play(Sound s);

}  // namespace boxpet::bsp