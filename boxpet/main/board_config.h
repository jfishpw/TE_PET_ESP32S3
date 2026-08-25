// board_config.h — 正点原子 ATK-DNESP32S3B0 (ESP32-S3R8) 引脚与硬件参数
// 引脚参考 xiaozhi-esp32 项目 boards/alientek/atk-dnesp32s3-box0/config.h
// 所有宏集中在此，便于跨 .cpp 共享。
#pragma once

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"

namespace boxpet {

// ===== 屏幕（ST7789 1.54 寸 IPS 240x240，SPI） =====
static constexpr gpio_num_t LCD_SCLK_PIN = GPIO_NUM_39;  // SPI2 SCLK
static constexpr gpio_num_t LCD_MOSI_PIN = GPIO_NUM_40;  // SPI2 MOSI
static constexpr gpio_num_t LCD_DC_PIN   = GPIO_NUM_38;
static constexpr gpio_num_t LCD_CS_PIN   = GPIO_NUM_41;
static constexpr gpio_num_t LCD_RST_PIN  = GPIO_NUM_NC;  // 本板无硬复位，走软件复位
static constexpr gpio_num_t LCD_BL_PIN   = GPIO_NUM_42;  // 背光（PWM 输出，LEDC 通道 0）
static constexpr ledc_channel_t    LCD_BL_LEDC_CH = LEDC_CHANNEL_0;
static constexpr int        LCD_PCLK_HZ = 40 * 1000 * 1000;  // 40MHz SPI（实测该屏稳定）
static constexpr int        LCD_HRES = 240;
static constexpr int        LCD_VRES = 240;
static constexpr int        LCD_OFFSET_X = 0;
static constexpr int        LCD_OFFSET_Y = 0;
static constexpr bool       LCD_MIRROR_X = false;
static constexpr bool       LCD_MIRROR_Y = false;
static constexpr bool       LCD_SWAP_XY = false;
// 本板 1.54 寸面板使用 BGR 像素序，与小智固件保持一致
static constexpr bool       LCD_BGR = true;

// ===== 按键（3 个物理按键） =====
// 左/右：低电平有效（按下为 0）；中键：BOOT(GPIO0) 风格高电平触发不可用，使用 GPIO4 高电平有效
static constexpr gpio_num_t BTN_LEFT_PIN  = GPIO_NUM_3;   // 按下=L，低电平有效
static constexpr gpio_num_t BTN_MID_PIN   = GPIO_NUM_4;   // 按下=H，高电平有效
static constexpr gpio_num_t BTN_RIGHT_PIN = GPIO_NUM_0;   // 按下=L，低电平有效（BOOT）
static constexpr bool       BTN_LEFT_ACTIVE_LOW  = true;
static constexpr bool       BTN_MID_ACTIVE_LOW   = false;
static constexpr bool       BTN_RIGHT_ACTIVE_LOW = true;

// ===== 音频（ES8311 codec + 板载喇叭） =====
static constexpr gpio_num_t AUDIO_I2S_MCLK_PIN = GPIO_NUM_13;
static constexpr gpio_num_t AUDIO_I2S_WS_PIN   = GPIO_NUM_10;
static constexpr gpio_num_t AUDIO_I2S_BCLK_PIN = GPIO_NUM_5;
static constexpr gpio_num_t AUDIO_I2S_DOUT_PIN = GPIO_NUM_6;
static constexpr gpio_num_t AUDIO_I2S_DIN_PIN  = GPIO_NUM_9;
static constexpr gpio_num_t AUDIO_CODEC_I2C_SDA_PIN = GPIO_NUM_11;
static constexpr gpio_num_t AUDIO_CODEC_I2C_SCL_PIN = GPIO_NUM_12;
static constexpr uint8_t    AUDIO_CODEC_ES8311_ADDR = 0x18;  // ES8311 7bit addr << 1
static constexpr gpio_num_t AUDIO_PA_ENABLE_PIN  = GPIO_NUM_21;  // 喇叭使能
static constexpr gpio_num_t AUDIO_CODEC_PWR_PIN = GPIO_NUM_14;  // CODEC 电源使能

// ===== 电源管理 =====
static constexpr gpio_num_t SYS_POW_PIN   = GPIO_NUM_2;   // 系统电源保持（保持 H）
static constexpr gpio_num_t CHG_CTRL_PIN  = GPIO_NUM_47;  // 充电控制（测量电池时拉低 100ms）
static constexpr gpio_num_t CHRG_PIN      = GPIO_NUM_48;  // 充电状态指示（低电平 = 接入 Type-C）
static constexpr gpio_num_t BAT_VSEN_PIN  = GPIO_NUM_1;   // 电池电压 ADC
static constexpr adc_unit_t BAT_ADC_UNIT  = ADC_UNIT_1;
static constexpr adc_channel_t BAT_ADC_CHANNEL = ADC_CHANNEL_0;  // GPIO1 → ADC1_CH0
// ATTEN_DB_12 量程约 0~3.3V；本板分压未知，先按 2:1（R_up=200k, R_down=100k，估测），
// 真实分压需要根据板子实际调整，或者改用查表法（参见 power.cpp 中的 kBatteryAdcTable）
static constexpr float BAT_VOLTAGE_DIVIDER_RATIO = 2.0f;  // Vbat = Vadc * 2.0

// ===== 板载 LED（双色 LED，绿 GPIO13，蓝 GPIO48）=====
// 本工程使用 LED 仅作为状态指示。
static constexpr gpio_num_t LED_GREEN_PIN = GPIO_NUM_13;

}  // namespace boxpet