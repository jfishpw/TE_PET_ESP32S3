// bsp/power.cpp — 电源管理（参考 xiaozhi-esp32 power_manager 实现）
//   * SYS_POW=GPIO2：开机后置高保持系统不掉电
//   * CODEC_PWR=GPIO14：拉高使能音频 codec 电源
//   * CHG_CTRL=GPIO47：拉低 → 100ms 测量电池电压 → 拉高
//   * CHRG=GPIO48：低电平 = Type-C 已接入
//   * BAT_VSEN=GPIO1：ADC1_CH0，ADC_ATTEN_DB_12
#include "power.h"
#include "board_config.h"

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

static const char* TAG = "power";

namespace boxpet::bsp {

namespace {

static adc_oneshot_unit_handle_t s_adc = nullptr;
static esp_timer_handle_t        s_timer = nullptr;
static PowerStatus               s_status = {};
static PowerEventCb              s_evt_cb = nullptr;
static void*                     s_evt_ctx = nullptr;
static bool                      s_low_warned = false;

// 标定依据：xiaozhi-esp32 BOX0 实测 raw↔电压线性关系（2951→3.80V，3231→4.20V，
// 约 700 ADC/V，含 CHG_CTRL 切换补偿）。
// 旧表把 0-100% 压缩在 3.80-4.20V（3.79V 即 0%），中低电量严重低估 → 改用
// 完整锂电放电曲线 3.40-4.20V。
struct AdcLut { uint16_t adc; uint8_t pct; };
static const AdcLut kBatteryAdcTable[] = {
    {2671,  0},  // 3.40V
    {2741,  5},  // 3.50V
    {2811, 10},  // 3.60V
    {2881, 20},  // 3.70V
    {2951, 40},  // 3.80V
    {3021, 60},  // 3.90V
    {3091, 80},  // 4.00V
    {3161, 90},  // 4.10V
    {3231,100},  // 4.20V
};
static constexpr int kBatteryLutCount = sizeof(kBatteryAdcTable) / sizeof(kBatteryAdcTable[0]);

static uint8_t lookup_battery_pct(uint16_t adc) {
    if (adc <= kBatteryAdcTable[0].adc) return 0;
    if (adc >= kBatteryAdcTable[kBatteryLutCount - 1].adc) return 100;
    for (int i = 0; i < kBatteryLutCount - 1; i++) {
        if (adc >= kBatteryAdcTable[i].adc && adc < kBatteryAdcTable[i+1].adc) {
            float r = float(adc - kBatteryAdcTable[i].adc) /
                      float(kBatteryAdcTable[i+1].adc - kBatteryAdcTable[i].adc);
            return uint8_t(kBatteryAdcTable[i].pct + r * (kBatteryAdcTable[i+1].pct - kBatteryAdcTable[i].pct));
        }
    }
    return 0;
}

static uint16_t read_battery_adc_average(int samples = 8) {
    // CHG_CTRL 拉低 100ms 切换到电池通路，再连续采样
    gpio_set_level(CHG_CTRL_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(100));
    uint32_t sum = 0;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) == ESP_OK) {
            sum += raw;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    gpio_set_level(CHG_CTRL_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
    return uint16_t(sum / samples);
}

static void poll_fn(void* /*arg*/) {
    // 1. 充电状态读取（低电平表示 Type-C 接入）
    bool charging_now = (gpio_get_level(CHRG_PIN) == 0);
    // 2. ADC 采样（计算电量）+ 指数平滑（防单次采样噪声导致显示跳变）
    uint16_t adc = read_battery_adc_average(6);
    uint8_t raw_pct = lookup_battery_pct(adc);
    uint8_t pct = (uint8_t)((s_status.battery_pct + raw_pct + 1) / 2);
    // 3. 更新状态
    bool changed = false;
    if (s_status.battery_pct != pct) { s_status.battery_pct = pct; changed = true; }
    if (s_status.charging != charging_now) { s_status.charging = charging_now; changed = true; }
    s_status.supply = charging_now ? PowerSupply::TypeC : PowerSupply::Battery;
    bool low = (pct <= 20) && !charging_now;
    if (low != s_status.low_voltage) { s_status.low_voltage = low; changed = true; }
    ESP_LOGD(TAG, "ADC=%u pct=%u charging=%d", adc, pct, charging_now);
    if (changed && s_evt_cb) s_evt_cb(s_status, s_evt_ctx);
}

}  // namespace

esp_err_t power_init() {
    // SYS_POW / CODEC_PWR 输出高电平保持
    gpio_config_t out_cfg = {};
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pin_bit_mask = (1ULL << SYS_POW_PIN) | (1ULL << AUDIO_CODEC_PWR_PIN) | (1ULL << CHG_CTRL_PIN);
    ESP_RETURN_ON_ERROR(gpio_config(&out_cfg), TAG, "gpio out");
    ESP_LOGI(TAG, "bc1: gpio out cfg done, setting latches...");
    gpio_set_level(SYS_POW_PIN, 1);
    // 电源锁存脚必须 hold：Light Sleep 时 IDF 会隔离所有 GPIO（置高阻），
    // SYS_POW 悬空后锁存电路缓慢放电（几十秒~几分钟，随机）→ 系统断电 →
    // POWERON 复位循环（reset reason=1，无 coredump；表现为"息屏约 30 秒后
    // 重启、时钟显示 00:00 后回到入睡时刻且不走、单击唤不醒"）。
    // gpio_hold_en 是 pad 级保持，Light Sleep 中持续输出高电平，软复位后仍有效。
    gpio_hold_en(SYS_POW_PIN);
    ESP_LOGI(TAG, "bc2: SYS_POW high (held)");
    gpio_set_level(AUDIO_CODEC_PWR_PIN, 1);
    // 同 SYS_POW：睡眠中保持 codec 供电。ES8311 断电丢配置且仅在开机初始化，
    // 若睡眠中被隔离掉电，唤醒后无声（且 I2S 时钟丢失会导致 codec lock loss）。
    // CHG_CTRL 不 hold：电池采样每 30s 需要动态拉低/拉高切换测量通路。
    gpio_hold_en(AUDIO_CODEC_PWR_PIN);
    ESP_LOGI(TAG, "bc3: CODEC_PWR high (held)");
    gpio_set_level(CHG_CTRL_PIN, 1);
    ESP_LOGI(TAG, "bc4: CHG_CTRL high");
    // CHRG 输入上拉
    gpio_config_t in_cfg = {};
    in_cfg.mode = GPIO_MODE_INPUT;
    in_cfg.pull_up_en = GPIO_PULLUP_ENABLE;
    in_cfg.pin_bit_mask = (1ULL << CHRG_PIN);
    ESP_RETURN_ON_ERROR(gpio_config(&in_cfg), TAG, "gpio in CHRG");
    ESP_LOGI(TAG, "bc5: CHRG input cfg done");

    // ADC 初始化（ADC1_CH0 = GPIO1）
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BAT_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_new_unit(&unit_cfg, &s_adc), TAG, "adc init");
    ESP_LOGI(TAG, "bc6: adc unit ok");
    adc_oneshot_chan_cfg_t ch_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    ESP_RETURN_ON_ERROR(adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &ch_cfg), TAG, "adc ch cfg");
    // 初始读一次
    s_status.battery_pct = lookup_battery_pct(read_battery_adc_average(4));
    s_status.charging = (gpio_get_level(CHRG_PIN) == 0);
    s_status.supply = s_status.charging ? PowerSupply::TypeC : PowerSupply::Battery;
    s_status.low_voltage = (s_status.battery_pct <= 20) && !s_status.charging;
    ESP_LOGI(TAG, "power init: pct=%u charging=%d", s_status.battery_pct, s_status.charging);
    return ESP_OK;
}

esp_err_t power_start_monitor() {
    if (s_timer) return ESP_OK;
    esp_timer_create_args_t t = {
        .callback = poll_fn,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "power_poll",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&t, &s_timer), TAG, "create timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(s_timer, 30ULL * 1000 * 1000), TAG, "start timer");  // 30s（5s 太频繁，CHG_CTRL 每 5s 切换 100ms+6 次 ADC ≈ 130ms 断续耗电）
    ESP_LOGI(TAG, "power monitor started (30s period)");
    return ESP_OK;
}

PowerStatus power_get_status() {
    return s_status;
}

void power_register_event_cb(PowerEventCb cb, void* ctx) {
    s_evt_cb = cb;
    s_evt_ctx = ctx;
}

void power_deinit() {
    if (s_timer) {
        esp_timer_stop(s_timer);
        esp_timer_delete(s_timer);
        s_timer = nullptr;
    }
    if (s_adc) {
        adc_oneshot_del_unit(s_adc);
        s_adc = nullptr;
    }
}

}  // namespace boxpet::bsp