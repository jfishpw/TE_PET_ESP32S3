// storage.cpp — NVS 存档实现
// namespace "boxpet" → NVS namespace "boxpet"
#include "storage.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cstring>

namespace boxpet::game {

static const char* TAG = "storage";
static const char* NVS_NAMESPACE = "boxpet";
static const char* NVS_KEY = "state";

static uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int j = 0; j < 8; ++j) {
            crc = (crc >> 1) ^ (0xEDB88320u & -(crc & 1));
        }
    }
    return ~crc;
}

esp_err_t storage_init() {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }
    nvs_close(nvs);
    ESP_LOGI(TAG, "storage ready");
    return ESP_OK;
}

esp_err_t storage_save(const PetState& s) {
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (err) return err;
    PetState tmp = s;
    // CRC 计算：除 crc 字段外的全部字节
    tmp.crc = 0;
    uint32_t c = crc32(reinterpret_cast<const uint8_t*>(&tmp), sizeof(PetState));
    PetState stored = s;
    stored.crc = c;
    err = nvs_set_blob(nvs, NVS_KEY, &stored, sizeof(PetState));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err) ESP_LOGE(TAG, "save failed: %s", esp_err_to_name(err));
    return err;
}

// 变更检测：任何"会随 tick 变化"的字段都参与 CRC（含 pet_seconds），
// 宠物活跃时几乎每 tick 都变 → 照常写；熄屏/无交互时属性衰减累积到
// 分钟级才有变化 → 大幅减少无谓写 Flash（NVS 磨损节流）。
static uint32_t s_last_crc = 0;   // 最近一次成功写入的状态 CRC（file-scope，供 erase 复位）
esp_err_t storage_save_if_changed(const PetState& s) {
    PetState tmp = s;
    tmp.crc = 0;
    uint32_t c = crc32(reinterpret_cast<const uint8_t*>(&tmp), sizeof(PetState));
    if (c == s_last_crc) return ESP_OK;            // 无变化：跳过
    esp_err_t err = storage_save(s);
    if (err == ESP_OK) s_last_crc = c;
    return err;
}

bool storage_load(PetState* out) {
    if (!out) return false;
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs) != ESP_OK) return false;
    size_t sz = sizeof(PetState);
    PetState tmp;
    esp_err_t err = nvs_get_blob(nvs, NVS_KEY, &tmp, &sz);
    nvs_close(nvs);
    if (err != ESP_OK || sz != sizeof(PetState)) {
        ESP_LOGW(TAG, "no save or size mismatch (err=%s)", esp_err_to_name(err));
        return false;
    }
    // 校验 CRC
    uint32_t saved_crc = tmp.crc;
    tmp.crc = 0;
    uint32_t c = crc32(reinterpret_cast<const uint8_t*>(&tmp), sizeof(PetState));
    if (c != saved_crc) {
        ESP_LOGE(TAG, "CRC mismatch: saved=0x%08lx calc=0x%08lx", saved_crc, c);
        return false;
    }
    tmp.crc = saved_crc;
    *out = tmp;
    ESP_LOGI(TAG, "loaded (stage=%d pstate=%d hunger=%d mood=%d)",
             (int)tmp.stage, (int)tmp.pstate, (int)tmp.hunger, (int)tmp.mood);
    return true;
}

void storage_erase() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_all(nvs);
    nvs_commit(nvs);
    nvs_close(nvs);
    s_last_crc = 0;   // 复位变更检测基线：重置后下一次保存必定落盘
    ESP_LOGI(TAG, "erased");
}

}  // namespace boxpet::game