// bsp/audio.cpp — ES8311 + I2S 音效合成器
//  链路：I2C(GPIO11/12) 配置 ES8311 → I2S STD TX(16kHz/16bit/mono) 输出 PCM
//        → ES8311 DAC → 喇叭（PA 使能 GPIO21）
//  audio_play() 非阻塞：音效进队列，后台任务合成方波+包络播放。
#include "audio.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c.h"
#include "driver/i2s_std.h"
#include "es8311.h"

#include "board_config.h"

namespace boxpet::bsp {

static const char* TAG = "audio";
static bool g_muted = false;

static i2s_chan_handle_t g_tx_chan = nullptr;
static es8311_handle_t g_es8311 = nullptr;
static QueueHandle_t g_sound_queue = nullptr;
static es8311_clock_config_t g_clk_cfg = {};   // 供睡眠唤醒后重初始化 codec
static constexpr int kCodecVolume = 62;        // 音量（es8311_init 软复位会清掉，需重设）

// --- 音符：频率 Hz / 时长 ms / 幅度（0~100） ---
struct Note { int freq; int ms; int amp; };
static constexpr int kNoteGapMs = 28;     // 音符间静音留白（拓麻歌子颗粒感）
static constexpr int kSampleRate = 16000; // 16kHz（已验证可出声的配置）
// 16k 采样奈奎斯特 8kHz：所有音符基频 ≤2637Hz，保证三次谐波 ≤7911Hz 不混叠。

static const Note* melody_of(Sound s, int* count) {
    // 拓麻歌子风格：压电片式清脆短哔声 —— 高频(1~2.6kHz)、短促、音符间留白。
    static const Note tick[]    = {{2093, 22, 30}};                              // 轻tick
    static const Note beep[]    = {{2093, 40, 45}};                              // 按键确认哔
    static const Note call[]    = {{2637, 45, 50}, {2637, 45, 50}, {2637, 45, 50}}; // 叫注意：三连哔
    static const Note feed[]    = {{1568, 35, 45}, {2093, 55, 45}};              // 吃：啊呜
    static const Note reject[]  = {{1046, 50, 40}, {784, 90, 40}};               // 拒绝：下行
    static const Note heal[]    = {{1318, 40, 45}, {1568, 40, 45}, {2093, 80, 50}}; // 痊愈上行
    static const Note flush_[]  = {{0, 220, 22}};                                // freq=0 → 白噪声(水声)
    static const Note win[]     = {{1046, 50, 45}, {1318, 50, 45}, {1568, 50, 45}, {2093, 140, 55}};
    static const Note lose[]    = {{784, 60, 40}, {523, 120, 40}};
    static const Note evolve[]  = {{1568, 55, 50}, {1976, 55, 50}, {2349, 55, 50}, {2637, 150, 55}};
    static const Note hatch[]   = {{1760, 40, 45}, {1760, 40, 45}, {2349, 100, 55}};
    static const Note die[]     = {{988, 90, 45}, {659, 90, 40}, {392, 160, 35}};// 下行哀乐
    static const Note correct[] = {{1976, 50, 50}, {2637, 90, 55}};
    static const Note wrong[]   = {{659, 130, 40}};
    static const Note sleep_[]  = {{1047, 60, 35}, {784, 110, 30}};              // 晚安下行
    static const Note shoot[]   = {{1568, 22, 42}, {1046, 55, 40}};              // 激光 pew：高→低

    const Note* m = tick; int n = 1;
    switch (s) {
        case Sound::Tick:    m = tick;    n = 1; break;
        case Sound::Beep:    m = beep;    n = 1; break;
        case Sound::Call:    m = call;    n = 3; break;
        case Sound::Feed:    m = feed;    n = 2; break;
        case Sound::Reject:  m = reject;  n = 2; break;
        case Sound::Heal:    m = heal;    n = 3; break;
        case Sound::Flush:   m = flush_;  n = 1; break;
        case Sound::Win:     m = win;     n = 4; break;
        case Sound::Lose:    m = lose;    n = 2; break;
        case Sound::Evolve:  m = evolve;  n = 4; break;
        case Sound::Hatch:   m = hatch;   n = 3; break;
        case Sound::Die:     m = die;     n = 3; break;
        case Sound::Correct: m = correct; n = 2; break;
        case Sound::Wrong:   m = wrong;   n = 1; break;
        case Sound::Sleep:   m = sleep_;  n = 2; break;
        case Sound::Shoot:   m = shoot;   n = 2; break;
    }
    *count = n;
    return m;
}

static void play_note(int freq, int ms, int amp_pct) {
    // 16kHz 采样，谐波合成波 + attack/release 包络。
    // 0.6·sin(f) + 0.25·sin(2f) + 0.15·sin(3f)：谐波分量模拟压电片清脆音色，
    // 音符基频 ≤2637Hz → 三次谐波 ≤7911Hz < 8kHz 奈奎斯特，无混叠。
    const int kMaxAmp = 19000;                     // 约 58% 满幅：实测 23000 在高音仍有轻微削波
    const int amp = kMaxAmp * amp_pct / 100;
    int total = kSampleRate * ms / 1000;
    if (total < 8) total = 8;
    int attack = total / 8;                        // 12% 起音
    int release = total / 3;                       // 33% 释放
    if (attack < 1) attack = 1;
    if (release < 1) release = 1;
    const int kChunk = 320;                        // 20ms 一块
    static int16_t pcm[2 * kChunk];
    const float kTwoPi = 6.28318530718f;
    const float phase_inc = freq > 0 ? kTwoPi * freq / kSampleRate : 0.0f;
    float phase = 0.0f;
    int prev_noise = 0;
    int t = 0;
    while (t < total) {
        int n = (total - t) < kChunk ? (total - t) : kChunk;
        for (int i = 0; i < n; ++i) {
            int idx = t + i;
            int v;
            if (freq == 0) {
                v = (int)(esp_random() & 0x1FFF) - 0x1000;    // ±4096
                v = (v + prev_noise * 3) / 4;                 // 一阶低通 → 沙沙水声
                prev_noise = v;
                v = v * amp_pct / 60;                          // 噪声额外压低
            } else {
                // 近纯正弦（实测唯一不破音的波形；谐波在 8Ω 小喇叭上失真发炸）
                v = (int)((0.90f * sinf(phase) + 0.07f * sinf(2.0f * phase)
                         + 0.03f * sinf(3.0f * phase)) * amp);
                phase += phase_inc;
                if (phase >= kTwoPi) phase -= kTwoPi;
            }
            // attack / release 包络
            int env_q15 = 32768;
            if (idx < attack)              env_q15 = 32768 * idx / attack;
            else if (idx >= total - release) env_q15 = 32768 * (total - idx) / release;
            int out = (int)(((int64_t)v * env_q15) >> 15);
            if (out >  30000) out =  30000;   // 软限幅保险（理论上到不了，防意外）
            if (out < -30000) out = -30000;
            pcm[2 * i]     = (int16_t)out;
            pcm[2 * i + 1] = (int16_t)out;
        }
        size_t written = 0;
        // 写超时（400ms）而非无限阻塞：实测 Light Sleep 唤醒后 DMA/ISR 可能
        // 遗留损坏，若无限等则音频任务卡死在 write 上，DMA 环上旧数据被反复
        // 输出 → 喇叭"不停重播"。超时即放弃本音符（掐断循环），下次睡眠/唤醒
        // 时通道被 disable+enable 彻底复位，音频随即恢复。
        esp_err_t werr = i2s_channel_write(g_tx_chan, pcm, (size_t)(2 * n) * sizeof(int16_t),
                                           &written, pdMS_TO_TICKS(400));
        if (werr != ESP_OK || written == 0) {
            t = total;   // 提前结束本音符
            break;
        }
        t += n;
    }
}

static void play_silence(int ms) {
    const int kChunk = 320;  // 20ms 一块
    static int16_t pcm[2 * kChunk];  // 全 0 静音
    int total = kSampleRate * ms / 1000;
    int t = 0;
    while (t < total) {
        int n = (total - t) < kChunk ? (total - t) : kChunk;
        size_t written = 0;
        esp_err_t werr = i2s_channel_write(g_tx_chan, pcm, (size_t)(2 * n) * sizeof(int16_t),
                                           &written, pdMS_TO_TICKS(400));
        if (werr != ESP_OK || written == 0) break;   // 同上：防 DMA 卡死无限重播
        t += n;
    }
}

static void audio_task(void*) {
    Sound s;
    for (;;) {
        if (xQueueReceive(g_sound_queue, &s, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (!g_muted && g_tx_chan) play_silence(20);  // 空闲持续写静音，防 DMA 环旧数据循环重播
            continue;
        }
        if (g_muted || !g_tx_chan) continue;
        int n = 0;
        const Note* m = melody_of(s, &n);
        for (int i = 0; i < n; ++i) {
            if (i > 0) play_silence(kNoteGapMs);         // 音符间留白 → 颗粒感
            play_note(m[i].freq, m[i].ms, m[i].amp);
        }
        play_silence(20);                                // 尾音冲刷
    }
}

esp_err_t audio_init() {
    // 1) PA 使能常开（实测唯一稳定出声结构；门控版本无声，具体见调试记录）。
    // 注意：若上次睡眠中异常复位（看门狗/panic），PA 脚可能还残留 hold 低，
    // hold 跨复位有效会挡住这里的置高 → 先解除再配置。
    gpio_hold_dis(AUDIO_PA_ENABLE_PIN);
    gpio_set_direction(AUDIO_PA_ENABLE_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(AUDIO_PA_ENABLE_PIN, 1);

    // 2) I2C master（ES8311 控制通道）
    i2c_config_t i2c_cfg = {};
    i2c_cfg.mode = I2C_MODE_MASTER;
    i2c_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
    i2c_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
    i2c_cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_cfg.master.clk_speed = 100000;
    i2c_cfg.clk_flags = 0;
    esp_err_t err = i2c_param_config(I2C_NUM_0, &i2c_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c_param_config: %s", esp_err_to_name(err)); return err; }
    err = i2c_driver_install(I2C_NUM_0, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2c_driver_install: %s", esp_err_to_name(err)); return err; }

    // 2) I2S STD TX：16kHz / 16bit / stereo（左右同数据）
    // 注意：不启用 auto_clear —— 实测此驱动版本 auto_clear=true 会无声。
    // 时钟连续性由 audio_task 空闲持续写静音保证（见 audio_task）。
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = 320;
    err = i2s_new_channel(&chan_cfg, &g_tx_chan, nullptr);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel: %s", esp_err_to_name(err)); return err; }

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg.mclk = AUDIO_I2S_MCLK_PIN;
    std_cfg.gpio_cfg.bclk = AUDIO_I2S_BCLK_PIN;
    std_cfg.gpio_cfg.ws   = AUDIO_I2S_WS_PIN;
    std_cfg.gpio_cfg.dout = AUDIO_I2S_DOUT_PIN;
    std_cfg.gpio_cfg.din  = GPIO_NUM_NC;
    std_cfg.gpio_cfg.invert_flags.mclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.bclk_inv = false;
    std_cfg.gpio_cfg.invert_flags.ws_inv = false;
    err = i2s_channel_init_std_mode(g_tx_chan, &std_cfg);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_channel_init_std_mode: %s", esp_err_to_name(err)); return err; }
    err = i2s_channel_enable(g_tx_chan);
    if (err != ESP_OK) { ESP_LOGE(TAG, "i2s_channel_enable: %s", esp_err_to_name(err)); return err; }

    // 3) ES8311：MCLK 自 MCLK 引脚（256×fs = 4.096MHz，已验证配置）
    g_es8311 = es8311_create(I2C_NUM_0, AUDIO_CODEC_ES8311_ADDR);
    if (!g_es8311) { ESP_LOGE(TAG, "es8311_create failed"); return ESP_FAIL; }
    g_clk_cfg.mclk_inverted = false;
    g_clk_cfg.sclk_inverted = false;
    g_clk_cfg.mclk_from_mclk_pin = true;
    g_clk_cfg.mclk_frequency = 256 * kSampleRate;
    g_clk_cfg.sample_frequency = kSampleRate;
    err = es8311_init(g_es8311, &g_clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (err != ESP_OK) { ESP_LOGE(TAG, "es8311_init: %s", esp_err_to_name(err)); return err; }
    es8311_voice_volume_set(g_es8311, kCodecVolume, nullptr);
    es8311_voice_mute(g_es8311, false);

    // 4) 播放队列 + 任务（队列短，避免音效积压后连响）
    g_sound_queue = xQueueCreate(4, sizeof(Sound));
    xTaskCreatePinnedToCore(audio_task, "audio", 4096, nullptr, 5, nullptr, 1);

    // 开机提示音（也用于自检：喇叭有声 = 链路 OK）
    audio_play(Sound::Beep);
    ESP_LOGI(TAG, "audio ready: ES8311 + I2S 16k, vol 62, PA on, sine");
    return ESP_OK;
}

void audio_set_muted(bool muted) { g_muted = muted; }
bool audio_is_muted() { return g_muted; }

// ===== Light Sleep 前后音频处理 =====
// 实测结论：
//   * PA 全程不可断电 —— 功放上电瞬间耦合电容充电 → 亮屏"破音"，
//     且 codec/功放上电未稳定 → 唤醒后无声/发闷。故 PA pad-hold 保持高电平。
//   * 喇叭"破音/杂音/无声"的真正根因：Light Sleep 停掉数字外设时钟，
//     MCLK 停止 → ES8311 DAC 时钟关系失锁；唤醒瞬间 MCLK 恢复时
//     DAC 输出直流跳变（破音），此后始终按错乱时钟输出（杂音/无声）。
// 对策：睡前软静音 DAC + 干净停掉 I2S，睡醒后完整重初始化 ES8311
//（软复位→重配时钟/格式→模拟上电）再解除静音，同时 I2S enable 恢复 DMA。
void audio_prepare_sleep() {
    // 1) 软静音 DAC：MCLK 停止瞬间无直流跳变（消除唤醒破音）
    if (g_es8311) es8311_voice_mute(g_es8311, true);
    // 2) 停 I2S：时钟干净停止；若有阻塞中的写操作会被释放返回（防 DMA 卡死）
    if (g_tx_chan) i2s_channel_disable(g_tx_chan);
    // 3) 丢弃积压音效（睡眠期间不播放，避免唤醒后连响）
    if (g_sound_queue) xQueueReset(g_sound_queue);
    // 4) PA pad-hold 保持高电平：不掉功放（避免上电耦合电容充电爆音）
    gpio_hold_en(AUDIO_PA_ENABLE_PIN);
}

void audio_resume_from_sleep() {
    gpio_hold_dis(AUDIO_PA_ENABLE_PIN);
    if (g_tx_chan) {
        // 先 disable 再 enable 彻底重启 I2S：Light Sleep 恢复后仅 enable 会
        // 遗留 DMA 指针/ISR 状态损坏（表现为唤醒后第一次声音"不停重播"）。
        // disable 会复位 DMA 描述符与消息队列，enable 从干净状态重新起播。
        i2s_channel_disable(g_tx_chan);   // 若已为 READY 返回 INVALID_STATE，可忽略
        i2s_channel_enable(g_tx_chan);
    }
    // 等 MCLK 稳定几个周期再动 codec（ADPLL/分频锁定）
    vTaskDelay(pdMS_TO_TICKS(20));
    // 完整重初始化 ES8311，恢复睡眠期间丢失的 DAC 锁相（否则唤醒后无声/杂音）
    if (g_es8311) {
        es8311_init(g_es8311, &g_clk_cfg, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
        es8311_voice_volume_set(g_es8311, kCodecVolume, nullptr);
        es8311_voice_mute(g_es8311, false);
    }
}

void audio_play(Sound s) {
    if (g_muted) return;
    if (!g_sound_queue) return;
    // 节流：同一音效 300ms 内只播一次（防止事件重复派发听感"反复响"）
    static int64_t last_ms[16] = {0};
    int64_t now_ms = esp_timer_get_time() / 1000;
    int idx = (int)s;
    if (idx >= 0 && idx < 16) {
        if (now_ms - last_ms[idx] < 300) return;
        last_ms[idx] = now_ms;
    }
    xQueueSend(g_sound_queue, &s, 0);  // 队列满则丢弃（避免阻塞调用方）
}

}  // namespace boxpet::bsp
