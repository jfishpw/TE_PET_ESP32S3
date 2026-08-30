// power_mgr.cpp — 背光超时 + 睡眠联动 + Light Sleep 睡眠任务 + 深休眠
#include "power_mgr.h"
#include "board.h"
#include "board_config.h"
#include "audio.h"
#include "power.h"
#include "wallclock.h"
#include "game/pet_def.h"
#include "game/storage.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_pm.h"
#include "esp_attr.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
// RTC 计数器（深休眠期间照走，用于计算实际睡眠秒数）。
// v5.4 无公共 esp_rtc.h，该函数声明在 esp_hw_support 的按芯片 soc 头中。
#include "soc/esp32s3/rtc.h"

namespace boxpet::bsp {

namespace {

// 【调试开关】1 = 插着 USB 也允许 Light Sleep（复现断电循环时用）。正式版必须为 0
#define SLEEP_ALLOW_USB 0

using ::boxpet::game::PetClock;
using ::boxpet::game::pet_clock_from_seconds;
using ::boxpet::game::is_sleeping_hour;

static const char* TAG = "power_mgr";
static ::boxpet::game::PetCore* g_pet = nullptr;
static esp_timer_handle_t g_pm_bl_timer = nullptr;
static int64_t g_last_input_ms = 0;
static bool g_backlight_off = false;
static int64_t g_backlight_off_since_ms = 0;   // 熄屏起始时刻（0 = 亮屏中）
static constexpr int64_t kBacklightTimeoutMs = 10 * 1000;  // 10s 无操作熄屏（需求）
static constexpr int64_t kSleepBacklightMs   = 10 * 1000;  // 睡眠时段也是 10s
static constexpr int64_t kAlertWakeMinOffMs  = 30 * 1000;  // 提醒亮屏门槛：熄屏≥30s
static constexpr int64_t kMaxLightSleepSec   = 4294;       // esp_sleep_enable_timer_wakeup 上限
static constexpr int64_t kWakeGraceMs        = 500;        // GPIO 唤醒后吞键宽限期

static WakePredictor g_wake_predictor = nullptr;   // ui_main 注册的事件预测器
static int64_t g_grace_until_ms = 0;              // 吞键宽限期截止时刻（0 = 无）

// ===== 深休眠状态（RTC_NOINIT：深休眠期间保持，重启后仍可读）=====
static constexpr uint32_t kDsMagic = 0x42E77A1Bu;
static RTC_NOINIT_ATTR uint32_t s_ds_magic = 0;    // kDsMagic = 刚深睡过
static RTC_NOINIT_ATTR uint8_t  s_ds_reason = 0;   // kDsReasonNight / kDsReasonBattery
static RTC_NOINIT_ATTR uint64_t s_ds_rtc_us = 0;   // 入睡时刻 esp_rtc_get_time_us()
static constexpr int64_t kBatteryDsSelfCheckSec = 30 * 60;  // 低电量每 30min 自检

// 熄屏期间 CPU 降频（DFS 40MHz）：仅影响"插 USB 无法 Light Sleep"的空转场景；
// 电池态会直接 Light Sleep（CPU 全停），降频仅是锦上添花。
static void set_cpu_throttle(bool throttle) {
    static bool s_mgr_throttled = false;   // 本模块当前是否已降频（与按键调用时序一致）
    if (throttle == s_mgr_throttled) return;
    esp_pm_config_t pm = {};
    pm.max_freq_mhz = 240;
    pm.min_freq_mhz = throttle ? 40 : 80;
    pm.light_sleep_enable = false;         // 睡眠统一由本模块睡眠任务负责
    if (esp_pm_configure(&pm) == ESP_OK) {
        s_mgr_throttled = throttle;
        ESP_LOGI(TAG, "cpu DFS min=%dMHz", pm.min_freq_mhz);
    }
}

// 快速读取充电脚（深休眠恢复早期 board/power 未初始化，单独配一次输入）
static bool charging_now_fast() {
    gpio_config_t in_cfg = {};
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    in_cfg.pin_bit_mask = (1ULL << CHRG_PIN);
    gpio_config(&in_cfg);
    return gpio_get_level(CHRG_PIN) == 0;   // 低=已接入 Type-C
}

static bool in_sleep_window_now() {
    if (!g_pet) return false;
    int h0 = 23, h1 = 6;
    g_pet->sleep_window(&h0, &h1);
    if (h0 == h1) return false;             // 全天清醒窗口
    int h = 0, m = 0, s = 0;
    wallclock_now(&h, &m, &s);
    if (h0 < h1) return h >= h0 && h < h1;
    return h >= h0 || h < h1;               // 跨午夜（如 23→6）
}

// 距起床点秒数（宠物睡眠窗口的结束小时），用于夜间深休眠定时唤醒
static int64_t secs_to_wake_hour() {
    int h0 = 23, h1 = 6;
    if (g_pet) g_pet->sleep_window(&h0, &h1);
    int h = 0, m = 0, s = 0;
    wallclock_now(&h, &m, &s);
    int64_t now_sec = (int64_t)h * 3600 + m * 60 + s;
    int64_t target = (int64_t)h1 * 3600;
    int64_t diff = target - now_sec;
    if (diff <= 0) diff += 86400;           // 跨天：下次到起床点
    return diff;
}

// ===== 深休眠入口（不返回）=====
// 预睡动作：停音频 → PA/codec 断电（深睡零静态电流；醒来=重启，audio_init
// 会完整重建）→ 存档 + 墙钟快照 → 记录 RTC 时间戳（跨深睡保持）→ 定时唤醒
// + 左右键唤醒 → esp_deep_sleep_start。
static void enter_deep_sleep(uint8_t reason) {
    // 停 I2S/DAC/清音效队列（空指针保护：恢复快路径早于 audio_init 也安全）
    audio_prepare_sleep();
    // 深睡=重启，无需保留 PA/codec 上电状态：全部断电省静态电流
    // （Light Sleep 的"唤醒爆破音"顾虑只适用于同进程唤醒，重启不适用）
    gpio_hold_dis(AUDIO_PA_ENABLE_PIN);
    gpio_set_level(AUDIO_PA_ENABLE_PIN, 0);       // PA 关闭
    gpio_hold_dis(AUDIO_CODEC_PWR_PIN);
    gpio_set_level(AUDIO_CODEC_PWR_PIN, 0);        // codec 断电（省 2~5mA）
    // SYS_POW 电源锁存：重新输出高并 pad-hold（防深睡期间 GPIO 隔离导致
    // 锁存脚悬空放电 → 断电重启循环；正常启动时 power_init 也会再 hold 一次）
    gpio_hold_dis(SYS_POW_PIN);
    gpio_config_t out = {};
    out.mode = GPIO_MODE_OUTPUT;
    out.pull_up_en = GPIO_PULLUP_DISABLE;
    out.pin_bit_mask = (1ULL << SYS_POW_PIN);
    gpio_config(&out);
    gpio_set_level(SYS_POW_PIN, 1);
    gpio_hold_en(SYS_POW_PIN);
    ESP_LOGI(TAG, "power rails: PA/codec off, SYS_POW held");

    // 存档 + 墙钟快照（防重启后时间回拨 >10 分钟）
    if (g_pet) storage_save_if_changed(g_pet->state());
    wallclock_force_snapshot();

    // RTC 时间戳（跨深睡保持，唤醒后算实际睡眠秒数）
    s_ds_rtc_us = esp_rtc_get_time_us();
    s_ds_reason = reason;
    s_ds_magic  = kDsMagic;

    // 唤醒源：RTC 定时（夜间=到起床点 / 低电量=30min 自检）
    int64_t dur_sec = (reason == kDsReasonNight) ? secs_to_wake_hour()
                                                 : kBatteryDsSelfCheckSec;
    if (dur_sec <= 0) dur_sec = 60;
    esp_sleep_enable_timer_wakeup((uint64_t)dur_sec * 1000000ULL);
    // 左右键低电平唤醒（RTC GPIO 0/3；中键高电平不支持 EXT1 统一模式，不参与
    // 深睡唤醒——Light Sleep 阶段三键均可唤醒，深睡仅左右键）
    esp_sleep_enable_ext1_wakeup_io((1ULL << BTN_LEFT_PIN) | (1ULL << BTN_RIGHT_PIN),
                                    ESP_EXT1_WAKEUP_ANY_LOW);
    // ESP32-S3 的 RTC_SLOW_MEM 域在深休眠中恒常供电（不可配置断电），
    // RTC_NOINIT 数据天然保留，无需 esp_sleep_pd_config 干预。

    ESP_LOGI(TAG, "Deep Sleep %lld sec (reason=%d)", (long long)dur_sec, (int)reason);
    esp_deep_sleep_start();   // 不再返回（唤醒即重启）
    ESP_LOGE(TAG, "deep sleep aborted (unexpected)");  // 理论不可达
}

static void set_backlight_safe(bool on) {
    if (on) {
        set_cpu_throttle(false);        // 亮屏恢复 80MHz 下限，交互流畅
        board_display_wake();           // 开 LCD + 背光
    } else {
        set_cpu_throttle(true);         // 熄屏降至 40MHz 下限（USB 空转时省电）
        board_display_sleep();          // 关 LCD DC/DC + 背光（最大省电）
    }
    g_backlight_off = !on;
    g_backlight_off_since_ms = on ? 0 : (esp_timer_get_time() / 1000);
    ESP_LOGI(TAG, "backlight %s", on ? "ON" : "OFF");
}

static void bl_timeout_cb(void* /*arg*/) {
    using ::boxpet::game::Stage;
    if (!g_pet) return;
    const auto& st = g_pet->state();
    int64_t now = esp_timer_get_time() / 1000;
    // 统一超时：睡眠时段/灯关/平时都是 10s 无操作熄屏。
    // 注意：灯关时不再"无条件立即熄屏"——否则用户每次按键亮屏后 5s 内
    // 又被本 timer 强行熄灭，表现为"无法操作、屏幕反复熄灭"（bug）。
    int64_t timeout = kBacklightTimeoutMs;
    if (st.stage != Stage::Egg && st.stage != Stage::Dead) {
        PetClock pc = pet_clock_from_seconds(st.pet_seconds, st.time_mode);
        if (is_sleeping_hour(pc.hour) || !st.light_on) {
            timeout = kSleepBacklightMs;
        }
    }
    if (now - g_last_input_ms >= timeout) {
        set_backlight_safe(false);
    }
}

// ===== Light Sleep 睡眠任务 =====
// 熄屏且无 USB 时进入 Light Sleep。独立于 esp_timer 回调执行——
// 在定时器分发循环里睡眠会阻断 pet_tick 补跳（宠物时间冻结）。
// 低优先级(1)：醒来后 esp_timer 任务先跑完补跳和事件分发，本任务再决定是否续睡。
static void sleep_task_fn(void* /*arg*/) {
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (!g_backlight_off) continue;              // 亮屏中不睡
        // USB 在线（充电中）不睡：直读充电检测脚（低=接入）。
        // power 状态 30s 才刷一次太迟；Light Sleep 会让 USB CDC 断连，
        // Windows 报"设备无法正常工作"，导致无法烧录/看日志。
        // 【调试开关】SLEEP_ALLOW_USB=1 时插 USB 也睡——用于复现
        // "拔 USB 熄屏后崩溃重启循环"（用户报告时间停在入睡时刻 +
        // 00:00 闪烁），配合串口日志 + coredump 定位。调试完改回 0。
#if SLEEP_ALLOW_USB
#else
        if (gpio_get_level(CHRG_PIN) == 0) continue;
#endif
        // 深休眠优先：夜间宠物睡眠（整夜睡到起床点）/ 电量≤10% 应急。
        // 命中则存档并 esp_deep_sleep_start（不返回，唤醒=重启走恢复快路径）；
        // 未命中（白天醒着闲置）走下面的 Light Sleep，保持随机事件触发。
        if (power_mgr_try_deep_sleep()) {
            // 不返回（esp_deep_sleep_start）；此处理论不可达
            continue;
        }
        // 宠物 SLEEPING 不再禁止 Light Sleep——当初禁睡是因为"关灯睡觉与
        // Light Sleep 冲突（实测死机）"，死机根因是 SYS_POW 未 hold 导致
        // GPIO 隔离期间电源锁存脚悬空放电断电（已用 gpio_hold_en 修复）。
        // 而且宠物睡觉恰恰是熄屏时间最长、最需要省电的时段（夜间整晚）。
        int64_t wake_sec = g_wake_predictor ? g_wake_predictor() : 60;
        power_mgr_enter_light_sleep(wake_sec);
        // RTC 醒来后回到循环顶：delay 500ms 让 esp_timer 先补跳 + 分发事件，
        // 若事件把屏幕点亮（wake_for_alert），下轮检查就不续睡了。
    }
}

}  // namespace

esp_err_t power_mgr_init(::boxpet::game::PetCore* pet) {
    g_pet = pet;
    g_last_input_ms = esp_timer_get_time() / 1000;
    esp_timer_create_args_t cfg = {
        .callback = bl_timeout_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "pm_bl_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &g_pm_bl_timer);
    esp_timer_start_periodic(g_pm_bl_timer, 5ULL * 1000 * 1000);  // 每 5s 检查熄屏超时

    // GPIO 唤醒：任意按键按下立即唤醒（左/右低电平有效，中键高电平有效）。
    // 这是 ESP32-S3 Light Sleep 的标准按键唤醒方式（gpio_wakeup_enable
    // 支持所有 IO 且每脚可独立设电平；ext0 只支持单个 RTC IO，三键覆盖不了）。
    // 注册一次即可，之后每次 esp_light_sleep_start() 都生效。
    ESP_ERROR_CHECK(gpio_wakeup_enable(BTN_LEFT_PIN,  GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(BTN_RIGHT_PIN, GPIO_INTR_LOW_LEVEL));
    ESP_ERROR_CHECK(gpio_wakeup_enable(BTN_MID_PIN,   GPIO_INTR_HIGH_LEVEL));
    ESP_ERROR_CHECK(esp_sleep_enable_gpio_wakeup());

    // 独立低优先级睡眠任务：熄屏且无 USB 时进入 Light Sleep。
    // 不放在 esp_timer 回调里做——那会在定时器分发循环内重入睡眠，
    // 阻断 pet_tick 补跳（宠物时间冻结）。低优先级保证醒来后
    // esp_timer 任务先跑完补跳和事件分发，睡眠任务再决定是否续睡。
    static TaskHandle_t s_sleep_task = nullptr;
    if (s_sleep_task == nullptr) {
        xTaskCreate(sleep_task_fn, "pm_sleep", 4096, nullptr, 1, &s_sleep_task);
    }
    ESP_LOGI(TAG, "power_mgr init done (Light Sleep + GPIO wakeup)");
    return ESP_OK;
}

void power_mgr_on_user_input() {
    g_last_input_ms = esp_timer_get_time() / 1000;
    if (g_backlight_off) {
        set_backlight_safe(true);
    }
}

bool power_mgr_is_backlight_off() { return g_backlight_off; }

int64_t power_mgr_backlight_off_ms() {
    if (!g_backlight_off) return 0;
    return esp_timer_get_time() / 1000 - g_backlight_off_since_ms;
}

void power_mgr_wake_for_alert() {
    if (!g_backlight_off) return;
    if (power_mgr_backlight_off_ms() < kAlertWakeMinOffMs) return;
    set_backlight_safe(true);
    // 视为一次用户输入：亮屏后按现有超时（10s）自动熄屏
    g_last_input_ms = esp_timer_get_time() / 1000;
}

// 临近事件点秒数：由 ui_main 注册的预测器在入睡前实时计算（pull 模式），
// 不再由 ui tick 推送（push 模式值会过期）
void power_mgr_set_wake_predictor(WakePredictor fn) {
    g_wake_predictor = fn;
}

bool power_mgr_wake_grace_active() {
    return g_grace_until_ms != 0
        && (esp_timer_get_time() / 1000) < g_grace_until_ms;
}

// ===== 深休眠对公共接口 =====
bool power_mgr_try_deep_sleep() {
    if (!g_pet || !g_backlight_off) return false;
    // 深休眠前置条件：无 USB（与 Light Sleep 一致，保 USB CDC/充电）
    if (gpio_get_level(CHRG_PIN) == 0) return false;
    // 条件1：真实模式 + 宠物睡眠中 + 当前处于睡眠窗口 → 睡到起床点（整夜
    // 深度睡眠，夜间不触发任何事件——check_special_events 睡眠短路 + 补跳
    // 期事件开关关闭）。演示模式夜间极短（约 30 分钟），不值得深睡重启。
    const bool night = g_pet->state().time_mode == ::boxpet::game::TimeMode::Real
                    && g_pet->is_sleeping()
                    && in_sleep_window_now();
    // 条件2：电量 ≤10% 且未充电 → 应急深睡（每 30min 自醒自检 + 左右键唤醒）
    const bool lowbat = power_get_status().battery_pct <= 10;
    if (night)        { enter_deep_sleep(kDsReasonNight);   return true; }  // 不返回
    if (lowbat)       { enter_deep_sleep(kDsReasonBattery); return true; }  // 不返回
    return false;
}

bool power_mgr_deep_sleep_resume(int64_t* elapsed_sec, uint8_t* reason, bool* charging) {
    // 唤醒源守卫：冷启动/普通重启的唤醒原因是 POWERON 等，直接排除——
    // RTC_NOINIT 内存冷启动时内容随机，仅凭魔法值不足以区分（万一碰巧相等）。
    auto cause = esp_sleep_get_wakeup_cause();
    if (cause != ESP_SLEEP_WAKEUP_TIMER && cause != ESP_SLEEP_WAKEUP_EXT1) {
        s_ds_magic = 0;
        return false;
    }
    // RTC_NOINIT 魔法不匹配 = 非深睡唤醒 → 不处理
    if (s_ds_magic != kDsMagic) return false;
    s_ds_magic = 0;   // 一次性消费：本次启动只走一次恢复流程
    // 实际睡眠时长 = RTC 计数器差（深睡期间 RTC 照走）
    uint64_t now_us = esp_rtc_get_time_us();
    int64_t us = (now_us > s_ds_rtc_us) ? (int64_t)(now_us - s_ds_rtc_us) : 0;
    int64_t sec = us / 1000000;
    if (sec > 7 * 86400) sec = 7 * 86400;   // 防御上限：7 天
    if (sec < 0) sec = 0;
    if (elapsed_sec) *elapsed_sec = sec;
    if (reason)     *reason     = s_ds_reason;
    if (charging)   *charging   = charging_now_fast();
    return true;
}

void power_mgr_reenter_deep_sleep(uint8_t reason) {
    enter_deep_sleep(reason);   // 不返回
}

bool power_mgr_in_pet_sleep_window() {
    return g_pet && in_sleep_window_now();
}

void power_mgr_enter_light_sleep(int64_t wake_after_sec) {
    if (wake_after_sec <= 0) wake_after_sec = 1;
    if (wake_after_sec > kMaxLightSleepSec) wake_after_sec = kMaxLightSleepSec;
    // 1. RTC timer wakeup：到点必醒（保证提醒事件不漏）
    esp_sleep_enable_timer_wakeup((uint64_t)wake_after_sec * 1000000ULL);
    // 2. GPIO 唤醒：三个按键已在 init 时注册（gpio_wakeup_enable ×3 +
    //    esp_sleep_enable_gpio_wakeup），此处直接入睡即可。
    ESP_LOGI(TAG, "Light Sleep for %lld sec (backlight_off=%d)",
             (long long)wake_after_sec, (int)g_backlight_off);
    // 关键：进入 Light Sleep。醒来后从该函数返回。
    // esp_timer 默认 RTC 驱动 → 自动补跳所有堆积 tick
    // 睡前关 PA（喇叭功放）：GPIO 隔离期间 pad-hold 低电平，
    // 省掉功放静态电流（2~5mA，睡眠期间的占比大头之一）
    audio_prepare_sleep();
    esp_light_sleep_start();
    audio_resume_from_sleep();
    // ===== 醒来 =====
    auto cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Light Sleep wakeup (cause=%d)", (int)cause);
    // 按键唤醒：先开 500ms 吞键宽限期，再亮屏（顺序很重要——btn_scan 优先级
    // 更高，可能在亮屏之后才采样到边沿；先设宽限期保证中键唤醒一定被吞）。
    // 只吞中键（确认键），左/右键唤醒后立即响应操作（buttons.cpp 区分处理）。
    if (cause == ESP_SLEEP_WAKEUP_GPIO) {
        int64_t now = esp_timer_get_time() / 1000;
        g_last_input_ms  = now;
        g_grace_until_ms = now + kWakeGraceMs;
        set_backlight_safe(true);
    } else if (cause == ESP_SLEEP_WAKEUP_TIMER) {
        // RTC 定时唤醒：不亮屏（无提醒事件则继续睡），仅清宽限期防误吞
        g_grace_until_ms = 0;
    }
}

}  // namespace boxpet::bsp