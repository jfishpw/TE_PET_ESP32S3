// bsp/buttons.cpp — 3 物理按键轮询 + 两级消抖 + 长按检测
// 设计：100Hz 纯轮询任务。原始电平须连续稳定 ≥20ms 才确认为跳变，
// 确认按下 >1500ms 触发长按（每次按压至多一次），否则释放时触发短按。
// 机械抖动/长按中的电平毛刺不会产生重复事件。
#include "buttons.h"
#include "board_config.h"
#include "power_mgr.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

static const char* TAG = "buttons";

namespace boxpet::bsp {

namespace {

struct KeyState {
    // 两级消抖状态机：raw_last + edge_ms 确认电平；stable_pressed 才驱动事件。
    bool    raw_last;        // 上一次采样电平
    bool    stable_pressed;  // 已确认的按下状态
    int64_t edge_ms;         // 电平首次变化时间戳（持续稳定 kDebounceMs 才确认）
    int64_t press_ms;        // 确认按下的时刻（0 = 空闲）
    bool    long_fired;      // 本次按压是否已触发长按
    bool    swallow;         // 唤醒吞键：按下时屏幕为黑 → 本次按压只用于亮屏，不触发事件
    bool    active_low;
    gpio_num_t pin;
};

static KeyState s_keys[3] = {
    {false, false, 0, 0, false, false, BTN_LEFT_ACTIVE_LOW,  BTN_LEFT_PIN},
    {false, false, 0, 0, false, false, BTN_MID_ACTIVE_LOW,   BTN_MID_PIN},
    {false, false, 0, 0, false, false, BTN_RIGHT_ACTIVE_LOW, BTN_RIGHT_PIN},
};

static constexpr int64_t kDebounceMs  = 20;    // 电平稳定确认窗口
static constexpr int64_t kLongPressMs = 1500;  // 长按阈值

static KeyCallback g_cb = nullptr;
static TaskHandle_t s_scan_task = nullptr;
static volatile bool s_should_stop = false;

static bool read_level(gpio_num_t pin, bool active_low) {
    int raw = gpio_get_level(pin);
    return active_low ? (raw == 0) : (raw == 1);
}

static void scan_task_fn(void* arg) {
    (void)arg;
    // 初始化 raw_last 为当前电平，避免开机误触发
    for (int i = 0; i < 3; ++i) {
        s_keys[i].raw_last = read_level(s_keys[i].pin, s_keys[i].active_low);
        s_keys[i].edge_ms = esp_timer_get_time() / 1000;
    }
    while (!s_should_stop) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        for (int i = 0; i < 3; i++) {
            KeyState& ks = s_keys[i];
            // 1. 采样原始电平
            bool raw = read_level(ks.pin, ks.active_low);
            if (raw != ks.raw_last) {
                ks.raw_last = raw;
                ks.edge_ms  = now_ms;          // 新变化从现在开始计时
                // 边沿时刻快照屏幕状态：必须在第 4 步亮屏之前记录，
                // 否则消抖 20ms 确认按下时屏幕已亮，swallow 永远为 false。
                // 只吞"中键唤醒"（i==1=中键）；左/右键唤醒后立即可操作，
                // 这样状态页/游戏/设置页等子场景中按左键亮屏后第一次按键就能用。
                if (raw && i == 1) ks.swallow = power_mgr_is_backlight_off();
                else               ks.swallow = false;
            }
            // 2. 电平持续稳定 ≥ kDebounceMs 且与确认态不同 → 确认跳变
            if (raw == ks.raw_last && (now_ms - ks.edge_ms) >= kDebounceMs
                && raw != ks.stable_pressed) {
                ks.stable_pressed = raw;
                if (raw) {
                    // 确认按下（swallow 已在边沿时刻快照，见第 1 步）
                    ks.press_ms   = now_ms;
                    ks.long_fired = false;
                } else {
                    // 确认释放：短按（此前未触发长按且时长不足，且非唤醒键）
                    if (ks.press_ms > 0 && !ks.long_fired && !ks.swallow) {
                        int64_t dur = now_ms - ks.press_ms;
                        if (dur < kLongPressMs) {
                            if (g_cb) g_cb(static_cast<KeyId>(i), KeyEvent::ShortPress);
                        }
                    }
                    ks.press_ms   = 0;
                    ks.long_fired = false;
                    ks.swallow    = false;
                }
            }
            // 3. 长按判定：确认按住超过阈值，仅触发一次（唤醒键除外）
            if (ks.stable_pressed && ks.press_ms > 0 && !ks.long_fired
                && (now_ms - ks.press_ms) >= kLongPressMs) {
                ks.long_fired = true;
                if (!ks.swallow && g_cb) g_cb(static_cast<KeyId>(i), KeyEvent::LongPress);
            }
            // 4. 任意按键活动 → 重置背光超时
            if (raw != ks.stable_pressed || ks.stable_pressed) {
                boxpet::bsp::power_mgr_on_user_input();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));  // 100Hz 轮询
    }
    vTaskDelete(nullptr);
}

}  // namespace

esp_err_t buttons_init() {
    // 配置 3 个 GPIO 为输入（纯轮询 + 两级软件消抖，无需中断）
    // 拉电阻按极性配置：低电平有效 → 上拉；高电平有效（中键）→ 下拉，
    // 否则中键空闲被上拉成高电平 = 恒"按下"，开机会自动触发长按进设置。
    gpio_config_t cfg_lo = {};
    cfg_lo.intr_type = GPIO_INTR_DISABLE;
    cfg_lo.mode = GPIO_MODE_INPUT;
    cfg_lo.pin_bit_mask = (1ULL << BTN_LEFT_PIN) | (1ULL << BTN_RIGHT_PIN);
    cfg_lo.pull_up_en = GPIO_PULLUP_ENABLE;
    cfg_lo.pull_down_en = GPIO_PULLDOWN_DISABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg_lo), TAG, "gpio_config active-low");

    gpio_config_t cfg_hi = {};
    cfg_hi.intr_type = GPIO_INTR_DISABLE;
    cfg_hi.mode = GPIO_MODE_INPUT;
    cfg_hi.pin_bit_mask = (1ULL << BTN_MID_PIN);
    cfg_hi.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg_hi.pull_down_en = GPIO_PULLDOWN_ENABLE;
    ESP_RETURN_ON_ERROR(gpio_config(&cfg_hi), TAG, "gpio_config active-high");

    // 启动 scan 任务
    // 栈必须足够大：按键回调链会在本任务里直接执行 LVGL 刷新
    // （lv_label_set_text/布局/snprintf），实测 2048 会栈溢出
    // （coredump: "stack overflow in task btn_scan"）。
    s_should_stop = false;
    BaseType_t ok = xTaskCreate(scan_task_fn, "btn_scan", 6144, nullptr, 5, &s_scan_task);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "create scan task failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "buttons init done");
    return ESP_OK;
}

void buttons_set_callback(KeyCallback cb) {
    g_cb = std::move(cb);
}

}  // namespace boxpet::bsp