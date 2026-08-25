// storage.h — NVS 存档（PetState + CRC + 周期保存 + 损坏重置）
#pragma once

#include "esp_err.h"
#include "game/pet.h"

namespace boxpet::game {

esp_err_t storage_init();
esp_err_t storage_save(const PetState& s);  // 原子写
bool      storage_load(PetState* out);     // 成功 true；无存档/CRC 错返回 false
void      storage_erase();                 // 重置（设置菜单用）

}  // namespace boxpet::game