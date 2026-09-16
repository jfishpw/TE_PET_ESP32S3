// storage.h — NVS 存档（PetState + CRC + 周期保存 + 损坏重置）
#pragma once

#include "esp_err.h"
#include "game/pet.h"

namespace boxpet::game {

esp_err_t storage_init();
esp_err_t storage_save(const PetState& s);      // 原子写
// 变更检测存档：宠物状态未变化（CRC 相同）则跳过，避免 5 分钟周期定时器
// 在无交互时段空写 Flash（NVS 磨损节流）。变化后立即写。
esp_err_t storage_save_if_changed(const PetState& s);
bool      storage_load(PetState* out);     // 成功 true；无存档/CRC 错返回 false
void      storage_erase();                 // 重置（设置菜单用）

// ===== 玩/教 冷却（独立 NVS 键，避免改动 PetState 结构导致旧存档失效）=====
// 冷却以"宠物秒"计（与食物冷却一致：演示模式随之缩短）。
// 返回到期时刻（宠物秒）；0 或早于当前 = 无冷却。
int64_t   storage_get_cd(bool is_edu, int idx);
void      storage_set_cd(bool is_edu, int idx, int64_t expire_pet_sec);

}  // namespace boxpet::game