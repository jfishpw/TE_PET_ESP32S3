// bsp/board.h — 板级初始化（LCD + LVGL）
#pragma once

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_err.h"
#include <cstdint>

namespace boxpet::bsp {

// 初始化 SPI 总线、ST7789 面板、LVGL port 与背光。
// 必须在 buttons/power 初始化之前调用（因为 LVGL 需要 button event）。
// 调用成功后方可使用 ui_* 系列 API。
esp_err_t board_init();

// 关闭屏幕进入深度睡眠前的资源释放（暂未启用，保留接口）
void board_deinit();

// 背光开关（百分比 0~100）
void board_set_backlight(uint8_t percent);

// 整屏 sleep/wake（关显示 + 关背光；唤醒后再开）
// 用于熄屏时降低 ST7789 内部 DC/DC 耗电（约 5~10mA）
void board_display_sleep();
void board_display_wake();

// 切换到指定 LVGL 屏幕对象（原子，线程安全）
void board_load_screen(lv_obj_t* new_screen);

// 注册背光超时回调（电源管理）
using BacklightTimerCb = void (*)(void* ctx);
void board_register_backlight_timer_cb(BacklightTimerCb cb, void* ctx);

// 重置背光倒计时（任何按键触发）
void board_reset_backlight_timer();

}  // namespace boxpet::bsp