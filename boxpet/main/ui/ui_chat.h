// ui/ui_chat.h — 聊天浮层面板（需求5 修订版：不切换场景，覆盖在主界面上）
// 流程：主界面点"聊"→面板打开→按需连 WiFi→中键说话(≤10s)→识别→大模型→显示回复
// 操作：中键 开始/结束说话；长按中键 关闭面板（自动关 WiFi 省电）
#pragma once

#include "lvgl.h"
#include "game/pet.h"
#include "bsp/buttons.h"

namespace boxpet::ui {

// 打开/关闭面板（挂到当前活动屏，覆盖在主界面之上）
void chat_panel_open(::boxpet::game::PetCore* pet);
void chat_panel_close();
bool chat_panel_visible();

// 面板可见时，ui_main 按键先转发：返回 true 表示已消费
bool chat_panel_key(bsp::KeyId id, bsp::KeyEvent evt);

}  // namespace boxpet::ui