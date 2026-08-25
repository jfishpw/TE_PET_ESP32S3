// pet_event.h — 宠物核心向外发出的事件（订阅方：UI / 音频 / 存档）
#pragma once

#include <stdint.h>
#include "pet_def.h"

namespace boxpet::game {

enum class EventKind : uint8_t {
    // 操作反馈
    FeedOk         = 1,   // 喂食成功（v1=FoodKind）
    FeedRejected   = 2,   // 拒绝（v1=原因 0=饱 1=睡 2=抑郁 3=冷却 4=没库存）
    PettedOk       = 3,   // 抚摸成功
    BatheOk        = 4,   // 洗澡完成
    MedOk          = 5,   // 用药成功（v1=MedKind）
    MedRejected    = 6,
    SleepStart     = 7,   // 入睡
    WakeUp         = 8,   // 醒来（v1=0 自然 1=开灯 2=满精力）
    LightToggled   = 9,   // v1=on
    PlayStart      = 10,  // v1=PlayKind
    PlayFinished   = 11,  // v1=PlayKind v2=won
    EduStart       = 12,  // v1=EduKind
    EduFinished    = 13,  // v1=EduKind v2=正确数
    SkillLearned   = 14,  // v1=SkillId
    LevelUp        = 15,  // v1=新等级 v2=解锁提示 id
    Pooped         = 16,  // v1=新便便数

    // 状态变化
    StageChanged   = 20,  // v1=新 Stage
    EvoDecided     = 21,  // v1=EvoForm（少年→成熟结算）
    Sick           = 22,  // 生病
    Healed         = 23,  // 痊愈
    Overeat        = 24,  // 吃撑
    Depressed      = 25,  // 进入抑郁
    DepressCured   = 26,  // 抑郁解除
    Died           = 27,  // 死亡
    Dying          = 28,  // 濒死警告（health=0）
    Born           = 29,  // v1=本胎数量（繁育出生）
    GestationStart = 30,  // 进入孕育期
    Hatch          = 31,  // 蛋孵化

    // 特殊事件（需求 §5）：v1=SpecialEventId
    //   需要玩家选择的：UI 弹窗 → resolve_event(choice)
    SpecialEvent   = 40,
    EventResolved  = 41,  // v1=SpecialEventId v2=选择(0=左/默认 1=右)
    GiftReceived   = 42,  // v1=物品类型 0=食 1=药 v2=Kind
    RunawayWarn    = 43,  // 离家出走倒计时（v1=剩余秒）

    // 提醒
    AttentionFlash = 50,  // v1=bitmask（bit0饿 bit1心情 bit2病 bit3睡 bit4脏 bit5抑郁 bit6濒死）
    AutoSleepHint  = 51,  // 23:00 该睡觉提示
};

struct Event {
    EventKind kind;
    int       v1 = 0;
    int       v2 = 0;
};

}  // namespace boxpet::game
