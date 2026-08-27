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
    Shoot       = 15,  // 飞机射击（pew）
};

esp_err_t audio_init();
void      audio_set_muted(bool muted);
bool      audio_is_muted();
void      audio_play(Sound s);

// Light Sleep 前后音频处理（PA 全程不掉电，消除亮屏破音与唤醒后无声）：
// prepare_sleep：软静音 DAC → 停 I2S → 清音效队列 → PA pad-hold 保持高电平；
// resume_from_sleep：PA 解 hold → 重启 I2S → 完整重初始化 ES8311（软复位 +
// 重配时钟/格式 + 模拟上电）→ 恢复音量 → 解除静音。睡眠期间多耗 2~5mA
// 静态电流（PA 不掉电的代价），换取音效可靠、零爆音。
void      audio_prepare_sleep();
void      audio_resume_from_sleep();

}  // namespace boxpet::bsp