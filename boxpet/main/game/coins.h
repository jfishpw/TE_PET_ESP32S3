// game/coins.h — 全局金币经济系统（NVS 持久化 + 结算 API）
#pragma once

#include <cstdint>

namespace boxpet::game {

// 初始化：从 NVS 读取金币（无记录则从 0 开始）
void coins_init();

// 当前金币
int32_t coins_get();

// 增加金币（正数=获得；负数=消费，要求余额足够，否则不扣减返回 false）
bool coins_add(int32_t delta);
bool coins_spend(int32_t cost);

// 结算计算（按需求：玩耍/教育/飞机游戏 + 评价系数）
// 返回应奖励金币数；UI 调用 coins_add 后再展示飘字
int32_t calc_play_reward(bool won, float energy_remain_pct);  // 胜 +5×精力剩余比，败 +2
int32_t calc_edu_reward(int correct_count, int total_questions); // 全对+15；N对+3×N
int32_t calc_plane_reward(int hits, bool time_up); // 命中×1，60s 通关+50

}  // namespace boxpet::game