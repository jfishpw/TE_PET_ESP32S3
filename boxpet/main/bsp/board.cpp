// bsp/board.cpp — 板级初始化（ST7789 + LVGL port）
// 参考 espressif/esp-bsp esp_lvgl_port 组件和 xiaozhi-esp32 atk-dnesp32s3-box0 的实现。
#include "board.h"
#include "board_config.h"

#include "esp_err.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "board";

namespace boxpet::bsp {

// 背光 LEDC 配置（使用 LEDC 通道 0 控制 GPIO42）
static constexpr ledc_timer_bit_t BL_LEDC_RES   = LEDC_TIMER_8_BIT;  // 0~255
static constexpr ledc_mode_t      BL_LEDC_MODE  = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t     BL_LEDC_TIMER = LEDC_TIMER_0;
static constexpr int              BL_LEDC_FREQ  = 5000;  // 5kHz，避免人耳噪音

static esp_lcd_panel_io_handle_t g_panel_io = nullptr;
static esp_lcd_panel_handle_t    g_panel    = nullptr;
static lv_display_t*             g_display   = nullptr;
static uint8_t                   g_backlight = 100;
static BacklightTimerCb          g_bl_cb     = nullptr;
static void*                     g_bl_cb_ctx = nullptr;
static esp_timer_handle_t        g_bl_timer  = nullptr;
static constexpr uint32_t        kBacklightTimeoutMs = 90 * 1000;  // 90s

// 背光 LEDC 初始化（不影响 ES8311 I2S MCLK GPIO13）
static esp_err_t backlight_ledc_init() {
    ledc_timer_config_t tcfg = {};
    tcfg.duty_resolution = BL_LEDC_RES;
    tcfg.freq_hz         = BL_LEDC_FREQ;
    tcfg.speed_mode      = BL_LEDC_MODE;
    tcfg.timer_num       = BL_LEDC_TIMER;
    tcfg.clk_cfg         = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&tcfg), TAG, "ledc_timer_config");

    ledc_channel_config_t ccfg = {};
    ccfg.channel    = LCD_BL_LEDC_CH;
    ccfg.duty       = 0;
    ccfg.gpio_num   = LCD_BL_PIN;
    ccfg.speed_mode      = BL_LEDC_MODE;
    ccfg.timer_sel      = BL_LEDC_TIMER;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&ccfg), TAG, "ledc_channel_config");

    ESP_RETURN_ON_ERROR(ledc_set_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH, 0), TAG, "ledc_set_duty");
    ESP_RETURN_ON_ERROR(ledc_update_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH), TAG, "ledc_update_duty");
    return ESP_OK;
}

// 内部：把百分比映射成 8bit duty
static uint32_t percent_to_duty(uint8_t p) {
    if (p > 100) p = 100;
    return (uint32_t)p * 255 / 100;
}

// SPI 总线初始化（用于 LCD，SPI2_HOST 不与 PSRAM 冲突）
static esp_err_t lcd_spi_init() {
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = LCD_MOSI_PIN;
    buscfg.miso_io_num = GPIO_NUM_NC;
    buscfg.sclk_io_num = LCD_SCLK_PIN;
    buscfg.quadwp_io_num = GPIO_NUM_NC;
    buscfg.quadhd_io_num = GPIO_NUM_NC;
    buscfg.max_transfer_sz = LCD_HRES * LCD_VRES * sizeof(uint16_t);
    return spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO);
}

// ST7789 面板初始化（无 RST 脚走软复位）
static esp_err_t lcd_panel_init() {
    esp_lcd_panel_io_spi_config_t io_cfg = {};
    io_cfg.cs_gpio_num     = LCD_CS_PIN;
    io_cfg.dc_gpio_num     = LCD_DC_PIN;
    io_cfg.spi_mode        = 0;
    io_cfg.pclk_hz         = LCD_PCLK_HZ;
    io_cfg.trans_queue_depth = 7;
    io_cfg.lcd_cmd_bits    = 8;
    io_cfg.lcd_param_bits  = 8;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi(SPI2_HOST, &io_cfg, &g_panel_io),
                        TAG, "new panel io");

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = LCD_RST_PIN;  // GPIO_NUM_NC -> 软件复位
    panel_cfg.rgb_ele_order  = LCD_BGR ? LCD_RGB_ELEMENT_ORDER_BGR : LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.data_endian    = LCD_RGB_DATA_ENDIAN_BIG;  // 与小智固件一致，依赖 LVGL swap_bytes
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(g_panel_io, &panel_cfg, &g_panel),
                        TAG, "new st7789 panel");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_panel),  TAG, "panel reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_panel),   TAG, "panel init");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(g_panel, true), TAG, "invert");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(g_panel, true), TAG, "disp_on");
    return ESP_OK;
}

// LVGL port 初始化 + display 注册
static esp_err_t lvgl_port_init_and_register_display() {
    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port_cfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle      = g_panel_io,
        .panel_handle   = g_panel,
        .control_handle = nullptr,
        .buffer_size    = LCD_HRES * 20,        // 20 行 partial refresh
        .double_buffer  = false,                // ST7789 单屏足够流畅
        .trans_size     = 0,
        .hres           = LCD_HRES,
        .vres           = LCD_VRES,
        .monochrome     = false,
        .rotation =
            {
                .swap_xy = LCD_SWAP_XY,
                .mirror_x = LCD_MIRROR_X,
                .mirror_y = LCD_MIRROR_Y,
            },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags =
            {
                .buff_dma    = 1,        // LCD DMA 需要 8-bit 对齐的内存
                .buff_spiram = 0,
                .sw_rotate   = 0,
                .swap_bytes  = 1,        // 与 ST7789 RAMCTRL big-endian 配对
                .full_refresh = 0,
                .direct_mode  = 0,
            },
    };
    g_display = lvgl_port_add_disp(&disp_cfg);
    if (!g_display) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

// 背光超时定时器回调
static void backlight_timeout_cb(void* arg) {
    if (g_bl_cb) g_bl_cb(g_bl_cb_ctx);
}

esp_err_t board_init() {
    ESP_RETURN_ON_ERROR(backlight_ledc_init(), TAG, "backlight_ledc_init");
    ESP_LOGI(TAG, "bcB1: ledc ok");
    ESP_RETURN_ON_ERROR(lcd_spi_init(),        TAG, "lcd_spi_init");
    ESP_LOGI(TAG, "bcB2: spi ok");
    ESP_RETURN_ON_ERROR(lcd_panel_init(),      TAG, "lcd_panel_init");
    ESP_LOGI(TAG, "bcB3: panel ok");
    ESP_RETURN_ON_ERROR(lvgl_port_init_and_register_display(), TAG, "lvgl");
    ESP_LOGI(TAG, "bcB4: lvgl ok");
    // 默认开背光
    ESP_LOGI(TAG, "bcB5: turning on backlight");
    board_set_backlight(100);
    ESP_LOGI(TAG, "bcB6: backlight on");
    // 启动 90s 背光超时
    esp_timer_create_args_t tcfg = {
        .callback = backlight_timeout_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "bl_timer",
        .skip_unhandled_events = true,
    };
    ESP_RETURN_ON_ERROR(esp_timer_create(&tcfg, &g_bl_timer), TAG, "create bl_timer");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(g_bl_timer, kBacklightTimeoutMs * 1000ULL),
                        TAG, "start bl_timer");
    ESP_LOGI(TAG, "board init done");
    return ESP_OK;
}

void board_set_backlight(uint8_t percent) {
    g_backlight = percent;
    uint32_t duty = percent_to_duty(percent);
    ledc_set_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH, duty);
    ledc_update_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH);
}

// ST7789 进入/退出 sleep 模式：关闭显示扫描 + 内部 DC/DC，省 5~10mA。
// 注意 esp_lcd_panel_disp_on_off(false) 仅关闭显示输出，sleep 指令更彻底。
void board_display_sleep() {
    if (!g_panel) return;
    // 关显示（停止扫描）+ 关背光
    esp_lcd_panel_disp_on_off(g_panel, false);
    ledc_set_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH, 0);
    ledc_update_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH);
    ESP_LOGD(TAG, "display sleep");
}

void board_display_wake() {
    if (!g_panel) return;
    // 开显示 + 按保存的背光恢复
    esp_lcd_panel_disp_on_off(g_panel, true);
    uint32_t duty = percent_to_duty(g_backlight);
    ledc_set_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH, duty);
    ledc_update_duty(BL_LEDC_MODE, LCD_BL_LEDC_CH);
    ESP_LOGD(TAG, "display wake bl=%u", g_backlight);
}

void board_load_screen(lv_obj_t* new_screen) {
    if (!lvgl_port_lock(500)) return;
    lv_disp_load_scr(new_screen);
    lvgl_port_unlock();
}

void board_register_backlight_timer_cb(BacklightTimerCb cb, void* ctx) {
    g_bl_cb = cb;
    g_bl_cb_ctx = ctx;
}

void board_reset_backlight_timer() {
    // 简化：定时器是周期触发的，每次按键会重启延时由 cb 内部判断
    // 这里通过重新打开背光让 cb 重置"超时累计"
    if (g_backlight == 0) {
        board_set_backlight(100);
    }
}

void board_deinit() {
    if (g_bl_timer) {
        esp_timer_stop(g_bl_timer);
        esp_timer_delete(g_bl_timer);
        g_bl_timer = nullptr;
    }
    lvgl_port_remove_disp(g_display);
    lvgl_port_deinit();
}

}  // namespace boxpet::bsp