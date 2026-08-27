// game/coins.cpp — 全局金币实现
//   * 持久化：NVS namespace "coins"，key "balance"
//   * 写入策略：coins_add/spend 后立即 commit，避免掉电丢币
//   * 消费验证：spend 失败返回 false 且余额不变（事务语义）
#include "coins.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include <cmath>

namespace boxpet::game {

namespace {
static const char* TAG = "coins";
static const char* kNvsNs = "coins";
static const char* kNvsKey = "balance";
static int32_t g_balance = 0;
static bool    g_inited  = false;

void persist_locked() {
    // 假设调用方已保证原子性（coins_add/spend 内仅一次调用）
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i32(h, kNvsKey, g_balance);
    nvs_commit(h);
    nvs_close(h);
}
}  // namespace

void coins_init() {
    if (g_inited) return;
    g_inited = true;
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) {
        g_balance = 0;
        return;
    }
    int32_t v = 0;
    if (nvs_get_i32(h, kNvsKey, &v) == ESP_OK) {
        g_balance = v;
    } else {
        g_balance = 0;
    }
    nvs_close(h);
    ESP_LOGI(TAG, "init: coins=%ld", (long)g_balance);
}

int32_t coins_get() { return g_balance; }

bool coins_add(int32_t delta) {
    if (delta == 0) return true;
    int64_t next = (int64_t)g_balance + delta;
    if (next < 0) return false;           // 防溢出（不允许扣到负）
    if (next > 999999) next = 999999;    // 软上限（足够 6 位显示）
    g_balance = (int32_t)next;
    persist_locked();
    return true;
}

bool coins_spend(int32_t cost) {
    if (cost <= 0) return true;
    if (g_balance < cost) return false;
    g_balance -= cost;
    persist_locked();
    return true;
}

// ===== 结算公式（与设计稿一致）=====
int32_t calc_play_reward(bool won, float energy_remain_pct) {
    if (won) {
        // 胜利：5 × 剩余精力比例（0~1），最少 +1
        float r = energy_remain_pct;
        if (r < 0.0f) r = 0.0f;
        if (r > 1.0f) r = 1.0f;
        return std::max(1, (int)(5 * r + 0.5f));
    }
    return 2;  // 失败安慰
}

int32_t calc_edu_reward(int correct_count, int total_questions) {
    if (correct_count >= total_questions) return 15;  // 全对奖励
    return 3 * std::max(0, correct_count);            // N 对 +3×N
}

int32_t calc_plane_reward(int hits, bool time_up) {
    int32_t r = std::max(0, hits);
    if (time_up) r += 50;  // 60 秒通关奖励
    return r;
}

}  // namespace boxpet::game