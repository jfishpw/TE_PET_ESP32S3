// bsp/backlight.cpp — 占位（背光逻辑已在 board.cpp 中实现，保留此 .cpp 兼容未来切换）
// 保留为单独文件以便未来添加亮度渐变动画、夜间调暗等功能。
#include "board.h"

namespace boxpet::bsp {

// 当前实现直接转发到 board 的接口，未来可在此扩展。
void backlight_set(uint8_t percent) {
    board_set_backlight(percent);
}

}  // namespace boxpet::bsp