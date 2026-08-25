// power_mgr.cpp — 背光超时 + 睡眠联动 + 调频（封装）
#include "power_mgr.h"
#include "board.h"
#include "board_config.h"
#include "game/pet_def.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "driver/gpio.h"

namespace boxpet::bsp {

namespace {

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
    if (!st.light_on) {
        // 灯关：直接关背光
        set_backlight_safe(false);
        return;
    }
    int64_t now = esp_timer_get_time() / 1000;
    // 蛋 / 死亡阶段没有"睡眠熄屏"概念：只按 90s 无操作超时
    bool sleeping_stage = (st.stage != Stage::Egg && st.stage != Stage::Dead);
    PetClock pc = pet_clock_from_seconds(st.pet_seconds, st.time_mode);
    int64_t timeout = (sleeping_stage && is_sleeping_hour(pc.hour))
                          ? kSleepBacklightMs    // 睡眠时段：30s 无操作熄屏
                          : kBacklightTimeoutMs; // 平时：90s 无操作熄屏
    if (now - g_last_input_ms >= timeout) {
        set_backlight_safe(false);
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
    // 配置 GPIO ext0 唤醒：任意按键按下即可唤醒（任一按键按下 = 任一 GPIO 拉到非空闲电平）
    // 左/右键是低电平有效（按下=0），中键是高电平有效（按下=1）。三个 key 不可能用单 ext0 描述，
    // 这里选 ext1 多 GPIO 唤醒：可在 Light Sleep 中任一指定 GPIO 变化时唤醒。
    // esp_sleep_enable_ext1_wakeup_io() 要求同一 RTC IO bank，且全部电平方向一致。
    // 左/右键 GPIO3、GPIO0 同属 RTC 低电平唤醒；中键 GPIO4 是高电平有效。
    // 简化处理：唤醒由 Light Sleep 的 RTC timer 自动完成（pet_core 计算的最近事件时间）。
    // 按键唤醒在 board.cpp 的轮询任务（btn_scan）自动恢复——它每 10ms 轮询一次，从 Light Sleep
    // 唤醒后第一轮就会检测到按键动作，调用 power_mgr_on_user_input() 亮屏。
    ESP_LOGI(TAG, "power_mgr init done (Light Sleep supported)");
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

// 临近事件点秒数（包含 margin），从 ui_main 喂入
static int64_t g_next_event_sec = 0;
void power_mgr_set_next_event_sec(int64_t sec) {
    g_next_event_sec = sec;
}
int64_t power_mgr_get_next_event_sec() { return g_next_event_sec; }

void power_mgr_enter_light_sleep(int64_t wake_after_sec) {
    if (wake_after_sec <= 0) wake_after_sec = 1;
    if (wake_after_sec > kMaxLightSleepSec) wake_after_sec = kMaxLightSleepSec;
    // 1. RTC timer wakeup：到点必醒（保证提醒事件不漏）
    esp_sleep_enable_timer_wakeup((uint64_t)wake_after_sec * 1000000ULL);
    // 2. GPIO ext0 wakeup：左键/右键按下立即唤醒（均为低电平有效，RTC IO 可行）
    //    左=GPIO3、右=GPIO0 都是 RTC IO。这里选 GPIO3（低电平）作为 ext0。
    //    注意 ext0 仅 1 个 GPIO，所以右/中键不会通过 ext0 唤醒。补救：right key 用 RTC
    //    peripheral interrupt (esp_sleep_enable_ext1_wakeup) 不支持不同电平，仅留 ext0 监听左键。
    esp_sleep_enable_ext0_wakeup(BTN_LEFT_PIN, 0);
    ESP_LOGI(TAG, "Light Sleep for %lld sec (backlight_off=%d)",
             (long long)wake_after_sec, (int)g_backlight_off);
    // 关键：进入 Light Sleep。醒来后从该函数返回。
    // esp_timer 默认 RTC 驱动 → 自动补跳所有堆积 tick
    esp_light_sleep_start();
    // ===== 醒来 =====
    auto cause = esp_sleep_get_wakeup_cause();
    ESP_LOGI(TAG, "Light Sleep wakeup (cause=%d)", (int)cause);
    // 醒来后若是因为按键 → 立即开屏
    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        // 任意按键唤醒（这里只配了左键）→ 视为一次用户输入
        power_mgr_on_user_input();
    }
}

}  // namespace boxpet::bsp