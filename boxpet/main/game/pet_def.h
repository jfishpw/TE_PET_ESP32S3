// pet_def.h — BoxPet 数值表（需求文档 v2：7 属性 + 状态机 + 互动/成长/繁育/事件）
// 所有数值集中此处，数据驱动。
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

namespace boxpet::game {

// ===== 时间体系 =====
enum class TimeMode : uint8_t {
    Demo = 0,  // 演示模式：1 宠物日 = 3600 真实秒（24 倍压缩）
    Real = 1,  // 真实模式：1 宠物日 = 86400 真实秒
};
constexpr int64_t kSecondsPerPetDayDemo = 3600;
constexpr int64_t kSecondsPerPetDayReal = 86400;
inline int64_t seconds_per_pet_day(TimeMode m) {
    return m == TimeMode::Real ? kSecondsPerPetDayReal : kSecondsPerPetDayDemo;
}
// 演示模式时间压缩倍数（属性速率 × 此值）
constexpr float kDemoSpeedup = 24.0f;

// ===== 宠物状态机（需求 §1.3）=====
enum class PetStateKind : uint8_t {
    IDLE      = 0,   // 待机
    EATING    = 1,   // 进食中（锁定 3s）
    SLEEPING  = 2,   // 睡眠
    PLAYING   = 3,   // 玩耍中
    LEARNING  = 4,   // 教育中
    SICK      = 5,   // 生病
    DEAD      = 6,   // 死亡
    DEPRESSED = 7,   // 抑郁（心情归零）
    BATHING   = 8,   // 洗澡中
    EVOLVING  = 9,   // 进化中
    BREEDING  = 10,  // 繁育中（孕育期）
};

// ===== 生命周期阶段（v5 多阶段多分支进化：等级+日龄双门槛驱动）=====
// 路线：蛋 →(孵化) 幼生LV1 →(LV2+2天) 成长 →(LV3+5天) 成熟 →(LV4+10天) 完全体
//       →(60天) 老年（寿命/死亡机制不变）。设计文档：docs/evolution_design.md
enum class Stage : uint8_t {
    Egg      = 0,   // 蛋形态期：孵化中（蛋皮随机 1/4）
    Baby     = 1,   // 幼生期：LV1 起
    Juvenile = 2,   // 成长期：LV2 起
    Adult    = 3,   // 成熟期：LV3 起
    Ultimate = 4,   // 完全体：LV4 起（LV5 满级后保持）
    Senior   = 5,   // 老年期：完全体/成熟期后按日龄
    Dead     = 6,
};
// 进化双门槛常量：等级门槛 AND 最低饲养日龄（防速刷，兼顾作息系统）。
// 两条件都满足才进化；不满足则每 tick 复查，条件齐的瞬间立即触发。
constexpr int   kEvoLvGrowth         = 2;    // 幼生→成长 等级门槛
constexpr int   kEvoDayGrowth        = 2;    // 幼生→成长 最低日龄（宠物日）
constexpr int   kEvoLvMature         = 3;    // 成长→成熟
constexpr int   kEvoDayMature        = 5;
constexpr int   kEvoLvUltimate       = 4;    // 成熟→完全体
constexpr int   kEvoDayUltimate      = 10;
constexpr int   kStageSeniorStartDay = 60;   // →老年（寿命机制不变）
// 蛋孵化真实秒
constexpr int64_t kEggIncubationDemoSec = 90;
constexpr int64_t kEggIncubationRealSec = 3 * 3600;  // 3 小时（2~6h 中位）
inline int64_t egg_incubation_seconds(TimeMode m) {
    return m == TimeMode::Real ? kEggIncubationRealSec : kEggIncubationDemoSec;
}

// ===== 多阶段多分支进化（v5）=====
// 每阶段 3 种形态 = 力/魔/速三分支外观（每次进化重新判定分支，可换向）；
// 品质档位决定华丽度，不占素材——由 UI 层用光效/粒子表现。
enum class EvoBranch : uint8_t {
    None  = 0,
    Force = 1,   // 力量型（power 最大）
    Magic = 2,   // 魔法型（magic 最大）
    Speed = 3,   // 速度型（speed 最大）
};
enum class EvoQuality : uint8_t {
    Normal   = 0,   // 普通：三维和 <90，无特效
    Fine     = 1,   // 优秀：90..150，2 颗星光粒子环绕
    Splendid = 2,   // 华丽：>150，呼吸光晕 + 4 颗星光粒子
};
// 外观资源ID：蛋皮×4 + 幼生/成长/成熟/完全体 ×力魔速 + 老年（共 17 项）
enum class EvoLook : uint8_t {
    Egg0 = 0, Egg1, Egg2, Egg3,                  // 蛋纹 4 种（新蛋随机）
    BabyForce, BabyMagic, BabySpeed,             // 幼生期
    GrowthForce, GrowthMagic, GrowthSpeed,       // 成长期
    MatureForce, MatureMagic, MatureSpeed,       // 成熟期
    UltimateForce, UltimateMagic, UltimateSpeed, // 完全体
    Senior,                                      // 老年（沿用现有外观）
    Count,
};
constexpr float kEvoStatMax          = 100.0f;  // 分支值上限（超出转经验）
constexpr float kEvoOverflowExpRatio = 0.2f;    // 溢出转经验：5 点 = 1 经验
constexpr float kQualityFineSum      = 90.0f;   // 三维和 ≥90 → 优秀
constexpr float kQualitySplendidSum  = 150.0f;  // 三维和 >150 → 华丽

// 分支判定：取三值最大；平局优先 power > magic > speed；全 0 保底力量型。
inline EvoBranch evo_decide_branch(float power, float magic, float speed) {
    if (power >= magic && power >= speed) return EvoBranch::Force;
    if (magic >= speed)                   return EvoBranch::Magic;
    return EvoBranch::Speed;
}
// 品质判定：三维之和分档（进化时锁定，下次进化可升档）。
inline EvoQuality evo_decide_quality(float power, float magic, float speed) {
    float sum = power + magic + speed;
    if (sum > kQualitySplendidSum) return EvoQuality::Splendid;
    if (sum >= kQualityFineSum)    return EvoQuality::Fine;
    return EvoQuality::Normal;
}
// 阶段+分支 → 外观资源ID（蛋期蛋皮由 PetState.evo_look 单独保存）
inline EvoLook evo_look_for(Stage st, EvoBranch b) {
    switch (st) {
        case Stage::Baby:     return b == EvoBranch::Magic ? EvoLook::BabyMagic
                                   : b == EvoBranch::Speed ? EvoLook::BabySpeed
                                                           : EvoLook::BabyForce;
        case Stage::Juvenile: return b == EvoBranch::Magic ? EvoLook::GrowthMagic
                                   : b == EvoBranch::Speed ? EvoLook::GrowthSpeed
                                                           : EvoLook::GrowthForce;
        case Stage::Adult:    return b == EvoBranch::Magic ? EvoLook::MatureMagic
                                   : b == EvoBranch::Speed ? EvoLook::MatureSpeed
                                                           : EvoLook::MatureForce;
        case Stage::Ultimate: return b == EvoBranch::Magic ? EvoLook::UltimateMagic
                                   : b == EvoBranch::Speed ? EvoLook::UltimateSpeed
                                                           : EvoLook::UltimateForce;
        case Stage::Senior:   return EvoLook::Senior;
        default:              return EvoLook::Egg0;
    }
}
// 阶段名（状态页/toast 用）
inline const char* stage_name(Stage st) {
    switch (st) {
        case Stage::Egg:      return "蛋";
        case Stage::Baby:     return "幼生";
        case Stage::Juvenile: return "成长";
        case Stage::Adult:    return "成熟";
        case Stage::Ultimate: return "完全体";
        case Stage::Senior:   return "老年";
        default:              return "-";
    }
}
// 分支名（状态页/toast 用）
inline const char* branch_name(EvoBranch b) {
    switch (b) {
        case EvoBranch::Force: return "力量型";
        case EvoBranch::Magic: return "魔法型";
        case EvoBranch::Speed: return "速度型";
        default:               return "-";
    }
}

// ===== 属性范围 =====
constexpr float kStatMax      = 100.0f;   // hunger/mood/energy/hygiene/health
constexpr float kStatMin      = 0.0f;
constexpr int   kIntMax       = 999;
constexpr int   kBondMax      = 999;

// ===== 衰减速率（真实模式，每真实分钟；演示模式 ×24）=====
// 需求 §1.1：hunger -1/30min，mood -1/45min，hygiene -1/60min
constexpr float kRateHungerDecay   = 1.0f / 30.0f;  // /min
constexpr float kRateMoodDecay     = 1.0f / 45.0f;
constexpr float kRateHygieneDecay  = 1.0f / 60.0f;
constexpr float kRateEnergyDrain   = 0.15f;         // 清醒时 /min（0.25→0.15：降低损耗，
                                                    //  满精力可支撑 ~11h 清醒活动）
constexpr float kRateEnergyRecover = 4.0f;          // 睡眠时 /min（2.0→4.0：0→100 睡 25 分钟）
// 睡眠：hunger 衰减减半，mood/hygiene 冻结
constexpr float kSleepHungerFactor = 0.5f;
// 联动扣减（hunger=0）：health -2/h，mood -5/h（需求 §1.1）
constexpr float kRateHealthStarve  = 2.0f / 60.0f;
constexpr float kRateMoodStarve    = 5.0f / 60.0f;
// 卫生低触发生病：hygiene≤20 时每小时 30% 概率
constexpr int   kSickHygieneThreshold = 20;
constexpr int   kSickHygieneChancePerHour = 30;

// ===== 状态阈值 =====
constexpr float kDepressedMoodThreshold = 10.0f;   // mood≤10 → 抑郁
constexpr float kForceSleepEnergy       = 0.0f;    // energy=0 → 强制睡眠
constexpr float kWakeMinEnergy          = 60.0f;   // 白天手动开灯起床的精力门槛（睡眠改写：
                                                    //  不足则拒绝唤醒并提示"精力不足，无法起床"；
                                                    //  夜间窗内一律拒绝，无论精力多少）
constexpr float kFeedRejectHunger       = 95.0f;   // hunger≥95 拒绝喂食
constexpr float kPlayMinEnergy          = 15.0f;   // energy<15 不可玩耍
constexpr float kEduMinEnergy           = 20.0f;   // energy<20 学不动
// 视觉阈值（需求：低于 60 时主界面明显体现）：
//   心情<60 → 伤心（bad/scold 帧 + 泪滴图标）
//   饱食<60 → 很饿（scold 帧 + 鸡腿图标）
//   卫生<60 → 脏（sick 帧 + 臭气图标）
//   精力<60 → 很累（bad/scold 帧 + "Z" 图标）
// 注意：比较用严格小于（<），"低于60"不含 60 本身。
constexpr float kLowMoodIdleThreshold    = 60.0f;
constexpr float kLowHungerIdleThreshold  = 60.0f;
constexpr float kLowHygieneIdleThreshold = 60.0f;
constexpr float kIdleTiredEnergy         = 60.0f;

// ===== 喂食（需求 §2.1）=====
enum class FoodKind : uint8_t {
    Meal     = 0,  // 主食：+30 饱食 +2 心情
    Snack    = 1,  // 零食：+10 饱食 +10 心情，冷却 2h，连喂 3 次→吃撑
    Premium  = 2,  // 高级料理：+50 饱食 +15 心情 +1 智力，冷却 6h
    Favorite = 3,  // 最爱食物：+40 饱食 +25 心情 +5 亲密度，冷却 12h
    Spoiled  = 4,  // 腐败食物：-10 饱食 -20 心情，30% 生病
    Count    = 5,
};
struct FoodDef {
    const char* name;
    float hunger;
    float mood;
    int   int_gain;
    int   bond_gain;
    int   cooldown_pet_min;  // 冷却（宠物分钟）
    int   sick_chance_pct;
    // 进化分支成长（喂食联动，超出 100 溢出转经验）
    float grow_power;
    float grow_magic;
    float grow_speed;
};
constexpr FoodDef kFoods[(int)FoodKind::Count] = {
    /* Meal     */ {"主食", 30, 2,  0, 0, 0,   0,  2, 0, 0},   // 高蛋白正餐→力量+2
    /* Snack    */ {"零食", 10, 10, 0, 0, 120, 0,  1, 1, 1},   // 小奖励→三项各+1
    /* Premium  */ {"高级料", 50, 15, 1, 0, 360, 0,  1, 1, 0}, // 珍馐→力/魔各+1
    /* Favorite */ {"最爱", 40, 25, 0, 5, 720, 0,  1, 1, 0},   // 最爱→力/魔各+1
    /* Spoiled  */ {"腐败食", -10, -20, 0, 0, 0, 30, 0, 0, 0},
};
constexpr int kSnackOvereatCount = 3;      // 连续零食次数→吃撑
constexpr int kOvereatSickChance = 30;     // 吃撑状态吃任何东西 30% 生病？——按需求：吃撑本身是 debuff
constexpr int kDislikeChancePct  = 10;     // 随机不喜欢 -3 心情
constexpr float kOvereatHungerCap = 100.0f;

// 无限库存物品（不检查库存、不扣减、菜单显示"无限"）
// 主食无限；高级料/最爱/特效药为稀有品（访客+商人掉落）；零食/腐败食保持有限
constexpr bool kFoodInfinite[(int)FoodKind::Count] = { true, false, false, false, false };

// ===== 玩耍（需求 §2.3，适配三按键）=====
enum class PlayKind : uint8_t {
    Ball     = 0,  // 丢球（方向猜）：-5 精力 +8 心情
    Rhythm   = 1,  // 节奏（记忆跟拍）：-15 精力 +20 心情，全对 +1 智力
    Free     = 2,  // 自由玩耍（观看动画）：-3 精力 +5 心情
    Count    = 3,
};
struct PlayDef {
    const char* name;
    float energy_cost;
    float mood_gain;
    int   bond_gain;
    int   int_gain;      // 全对时
    int   cooldown_hours; // 冷却小时数（0 = 无冷却；随模式缩放：演示=宠物小时）
    int   unlock_level;
    // 进化分支成长（玩耍联动）
    float grow_power;
    float grow_magic;
    float grow_speed;
};
constexpr PlayDef kPlays[(int)PlayKind::Count] = {
    /* Ball     */ {"丢球",   5,  8,  0, 0, 0, 1, 0, 0, 2},   // 追球跑→速度+2
    /* Rhythm   */ {"节奏",  15, 20, 0, 1, 4, 4, 0, 2, 0},   // 韵律记忆→魔法+2；LV4 解锁；4 小时冷却
    /* Free     */ {"自由玩", 3,  5,  0, 0, 0, 1, 0, 0, 2},   // 撒欢跑动→速度+2
};
constexpr int kPlayTiredCount = 3;  // 连续 3 次后喘气提示

// ===== 药品（需求 §2.4）=====
enum class MedKind : uint8_t {
    Fever    = 0,  // 退烧药：health+30，解除 SICK
    Stomach  = 1,  // 胃药：解除吃撑，hunger-20
    Vitamin  = 2,  // 维生素：health+10，24 宠物小时免疫
    Special  = 3,  // 特效药：health 全恢复，解除一切 debuff
    Count    = 4,
};
struct MedDef {
    const char* name;
    float health_gain;   // <0 = 恢复满
    int   immunity_pet_hours;
};
constexpr MedDef kMeds[(int)MedKind::Count] = {
    /* Fever   */ {"退烧药", 30, 0},
    /* Stomach */ {"胃药",   0, 0},
    /* Vitamin */ {"维生素", 10, 24},
    /* Special */ {"特效药", -1, 0},
};
// 退烧药/胃药/维生素无限；特效药为稀有品（访客+商人掉落）
constexpr bool kMedInfinite[(int)MedKind::Count] = { true, true, true, false };

// ===== 教育（需求 §2.5，适配按键选择题）=====
enum class EduKind : uint8_t {
    Word     = 0,  // 认字：5 题，+3 智力/题全对
    Math     = 1,  // 算术：5 题，+3 智力/题全对
    Music    = 2,  // 音乐（记忆）：+5 智力
    Read     = 3,  // 自由阅读：+1 智力
    Counter  = 4,  // 计数器：亿/万/个级拨珠位值教学（无等级门槛），结束 +1 智力
    Count    = 5,
};
struct EduDef {
    const char* name;
    float energy_cost;
    int   int_gain_per_correct;
    int   unlock_level;
    float grow_magic;    // 进化分支成长（学习联动→魔法）
    int   cooldown_hours;// 冷却小时数（0 = 无冷却；随模式缩放）
};
constexpr EduDef kEdus[(int)EduKind::Count] = {
    /* Word     */ {"认字", 8,  3, 3, 2, 4},
    /* Math     */ {"算术", 8,  3, 5, 2, 4},
    /* Music    */ {"音乐", 10, 5, 5, 2, 4},
    /* Read     */ {"自由阅", 3,  1, 3, 2, 4},
    /* Counter  */ {"计数器", 1, 1, 0, 2, 0},  // 无等级/无冷却（位值教学随时可用）
};
constexpr int kEduQuestions        = 5;   // 每课程 5 题
constexpr int kEduSkillLearnChance = 20;  // 教育全对习得技能 %
constexpr int kPlaySkillLearnChance = 10; // 玩耍胜利习得技能 %

// ===== 抚摸（主界面"摸"）=====
constexpr float kPetMoodGain = 3.0f;
constexpr int   kPetBondGain = 1;
constexpr int   kDepressCurePets = 5;     // 抑郁时连续抚摸 5 次解除
constexpr float kDepressCureMood = 20.0f; // 解除后 mood +20

// ===== 等级/经验（需求 §3.3）=====
constexpr int kExpFeed   = 5;
constexpr int kExpPlay   = 10;
constexpr int kExpEdu    = 15;
constexpr int kExpEvent  = 20;
constexpr int kExpBreed  = 50;
constexpr int kMaxLevel  = 30;
inline int exp_to_next(int level) {
    if (level >= kMaxLevel) return INT32_MAX;
    return (int)(50.0f * powf((float)level, 1.5f));
}
// 关键解锁等级
constexpr int kUnlockSnackLv    = 2;
constexpr int kUnlockWordLv     = 3;
constexpr int kUnlockMathLv     = 5;
constexpr int kUnlockMusicLv    = 5;   // 8→5（宠物最高 5 级）
constexpr int kUnlockBreedLv    = 15;

// ===== 技能（需求 §3.4）=====
enum class SkillId : uint8_t {
    ShakeHand = 0,  // 握手
    RollOver  = 1,  // 翻滚
    PlayDead  = 2,  // 装死
    Dance     = 3,  // 跳舞
    Sing      = 4,  // 唱歌
    MathShow  = 5,  // 算数表演
    Count     = 6,
};
constexpr const char* kSkillNames[(int)SkillId::Count] = {
    "握手", "翻滚", "装死", "跳舞", "唱歌", "算数",
};

// ===== 繁育（需求 §4，AI 配种简化）=====
constexpr int   kBreedMinLevel        = 15;
constexpr int   kBreedMinBond         = 300;   // 双方亲密 ≥300 → +10%（简化为本体 bond）
constexpr int   kBreedSuccessBasePct  = 60;    // AI 配种基础成功率
constexpr int   kBreedMaxCount        = 3;     // 一生最多 3 次
constexpr int   kBreedCooldownPetDays = 7;     // 两次间隔 ≥7 宠物日
constexpr int   kGestationMinPetHours = 12;    // 孕育 12~24 宠物小时
constexpr int   kGestationMaxPetHours = 24;
constexpr int   kBreedMaxBabies       = 3;     // 单胎 1~3
constexpr int   kBreedParentMoodLoss  = 10;

// ===== 特殊事件（需求 §5）=====
enum class SpecialEventId : uint8_t {
    None     = 0,
    Visitor  = 1,  // 访客来了（欢迎/赶走）
    Rain     = 2,  // 下雨了（打伞出去/在家待着）
    Nightmare= 3,  // 做噩梦（抚摸安慰/不管）
    Birthday = 4,  // 生日（自动：全属性+10+礼物）
    Meteor   = 5,  // 流星雨（许愿：稀有食物）
    Runaway  = 6,  // 离家出走（限时追回）
    Merchant = 7,  // 神秘商人（交易/拒绝）
};
// 事件权重（无特殊条件时）
constexpr int kEventChancePerTickPct = 3;  // 每 60s tick 3%
// 需要选择的事件：UI 弹窗 左/右 选项，10s 超时默认
constexpr int kEventPopupTimeoutSec = 10;

// ===== 作息（需求 §2.7/§2.2B）=====
constexpr int kPetDaySleepHour = 23;  // 23:00 提示睡觉
constexpr int kPetDayWakeHour  = 6;   // 06:00 可醒
constexpr int kAutoSleepDelaySec = 30; // 提示后 30s 自动入睡

// ===== 死亡规则 =====
constexpr int kSickDeathPetHours  = 48;  // SICK 48 宠物小时未治 → 死亡
constexpr int kDyingDeathPetHours = 72;  // health=0 濒死 72 宠物小时 → 死亡
constexpr int kSeniorDeathChancePerDay = 5;  // 老年每日 5%（温和化）

// ===== 亲密度衰减：3 宠物日无互动 → 每宠物日 -5 =====
constexpr int kBondIdlePetDays = 3;
constexpr int kBondIdleLossPerDay = 5;

// ===== 便便（与卫生联动）=====
constexpr int kPoopMax = 4;
constexpr int kPoopHygienePenalty = 15;   // 每次排泄 hygiene -15
constexpr float kPoopSpawnPerPetHour = 1.0f / 6.0f;  // 平均 6 宠物小时 1 个（真实模式约 6h 一次）

// ===== 洗澡 =====
constexpr float kBatheHygieneGain = 100.0f;  // 洗澡恢复满卫生
constexpr float kBatheMoodGain    = 2.0f;

// ===== 经验换算/杂项 =====
constexpr int kPetHourSecDemo = 150;   // 演示模式 1 宠物小时
constexpr int kPetHourSecReal = 3600;

struct PetClock {
    int hour;    // 0..23
    int minute;  // 0..59
};
// 宠物时钟：1 宠物小时 = 1 宠物日 / 24
inline PetClock pet_clock_from_seconds(int64_t total_pet_seconds, TimeMode m) {
    const int64_t hour_len = seconds_per_pet_day(m) / 24;
    if (hour_len <= 0) return PetClock{8, 0};
    PetClock c{};
    c.hour = (int)(total_pet_seconds / hour_len) % 24;
    if (c.hour < 0) c.hour += 24;
    c.minute = (int)((total_pet_seconds % hour_len) * 60 / hour_len);
    if (c.minute < 0) c.minute += 60;
    return c;
}
inline bool is_sleeping_hour(int pet_hour) {
    return pet_hour >= kPetDaySleepHour || pet_hour < kPetDayWakeHour;
}

}  // namespace boxpet::game
