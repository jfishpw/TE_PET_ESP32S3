// power_mgr.cpp — 背光超时 + 睡眠联动 + Light Sleep 睡眠任务
#include "power_mgr.h"
#include "board.h"
#include "board_config.h"
#include "game/pet_def.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static void set_backlight_safe(bool on) {
    if (on) {
        board_display_wake();      // 开 LCD + 背光
    } else {
        board_display_sleep();     // 关 LCD DC/DC + 背光（最大省电）
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
        // 宠物 SLEEPING 时不睡：关灯睡觉与 Light Sleep 冲突（实测会死机）
        if (g_pet) {
            using ::boxpet::game::PetStateKind;
            if (g_pet->state().pstate == PetStateKind::SLEEPING) continue;
        }
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
    esp_light_sleep_start();
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