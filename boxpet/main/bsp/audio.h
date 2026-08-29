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

// ===== 麦克风录音（需求5：ES8311 ADC，16k/16bit/mono）=====
// buf 由调用方分配（建议 PSRAM，10s 需 44+32000*2 字节）；
// 前 44 字节保留，stop 时写入 WAV 头，PCM 写入其后。
bool    audio_record_start(int16_t* buf, size_t max_samples);
size_t  audio_record_stop();    // 停止采集并返回 PCM 采样数
bool    audio_recording();
// 最近一次录音的错误描述（通道/数据异常时用于 UI 提示；无错误返回空串）
const char* audio_record_last_error();

// ===== 流式录音（小智对话，路径B）=====
// 持续采集 16k/16bit/mono，每 60ms(960 样本) 回调一帧。停止后不再回调。
typedef void (*AudioFrameCb)(const int16_t* pcm, size_t samples);
bool audio_record_stream_start(AudioFrameCb cb);
bool audio_record_stream_stop();

// ===== 流式播放（小智对话）：直接写 TX，语音会话期间暂停音效 =====
void audio_stream_begin();                          // 打开语音会话：暂停音效队列
void audio_stream_pcm(const int16_t* pcm, size_t n); // 播放服务器 PCM（16k mono）
void audio_stream_end();                            // 结束：恢复音效

}  // namespace boxpet::bsp