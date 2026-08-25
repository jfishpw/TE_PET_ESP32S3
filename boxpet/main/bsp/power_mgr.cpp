// power_mgr.cpp — 背光超时 + 睡眠联动 + 调频（封装）
#include "power_mgr.h"
#include "board.h"
#include "game/pet_def.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace boxpet::bsp {

namespace {

using ::boxpet::game::PetClock;
using ::boxpet::game::pet_clock_from_seconds;
using ::boxpet::game::is_sleeping_hour;

static const char* TAG = "power_mgr";
static ::boxpet::game::PetCore* g_pet = nullptr;
static esp_timer_handle_t g_bl_timer = nullptr;
static int64_t g_last_input_ms = 0;
static bool g_backlight_off = false;
static int64_t g_backlight_off_since_ms = 0;   // 熄屏起始时刻（0 = 亮屏中）
static constexpr int64_t kBacklightTimeoutMs = 90 * 1000;
static constexpr int64_t kSleepBacklightMs   = 30 * 1000;
static constexpr int64_t kAlertWakeMinOffMs  = 30 * 1000;  // 提醒亮屏门槛：熄屏≥30s

static void set_backlight_safe(bool on) {
    board_set_backlight(on ? 100 : 0);
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
        .name = "bl_timer",
        .skip_unhandled_events = true,
    };
    esp_timer_create(&cfg, &g_bl_timer);
    esp_timer_start_periodic(g_bl_timer, 5ULL * 1000 * 1000);  // 每 5s 检查
    ESP_LOGI(TAG, "power_mgr init done");
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
    // 视为一次用户输入：亮屏后按现有超时（90s/睡眠时段30s）自动熄屏
    g_last_input_ms = esp_timer_get_time() / 1000;
}

}  // namespace boxpet::bsp