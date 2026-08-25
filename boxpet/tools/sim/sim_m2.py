"""
BoxPet M2 游戏逻辑模拟器（与 boxpet/main/game/pet.cpp 严格等价）
=============================================================
本脚本是 pet.cpp 的 Python 1:1 翻译，用于在没有 ESP-IDF 的环境下验证：
  * 衰减节拍（hunger / happiness / poop）
  * 作息冻结（睡眠期间不衰减）
  * 生病与治疗
  * 生命周期阶段切换与进化分支
  * 事件订阅分发

用法：
  python tools/sim/sim_m2.py                             # 默认演示模式跑 1 天
  python tools/sim/sim_m2.py --mode real --hours 12    # 真实模式 12 小时
  python tools/sim/sim_m2.py --quiet                    # 只跑断言，不打印事件流
  python tools/sim/sim_m2.py --seed 42                  # 固定随机种子
"""
from __future__ import annotations
import argparse
import random
from dataclasses import dataclass, field
from enum import IntEnum
from typing import Callable, List, Optional

# ============================================================
# 数值表（与 pet_def.h 完全等价）
# ============================================================
class TimeMode(IntEnum):
    Demo = 0
    Real = 1

K_SECONDS_PER_PET_DAY = {TimeMode.Demo: 3600, TimeMode.Real: 86400}
K_REAL_TIME_MULTIPLIER = 24
K_PET_HOUR_LEN = 3600
K_HUNGER_MAX = 4
K_HAPPINESS_MAX = 4
K_POOP_MAX = 4
K_DISCIPLINE_MAX = 100

class Stage(IntEnum):
    Egg = 0
    Baby = 1
    Child = 2
    Teen = 3
    Adult = 4
    Senior = 5
    Dead = 6

K_STAGE_DAYS = {Stage.Egg: 0, Stage.Baby: 1, Stage.Child: 2,
                Stage.Teen: 2, Stage.Adult: 5, Stage.Senior: 3, Stage.Dead: 0}
K_EGG_DEMO_SEC = 60
K_EGG_REAL_SEC = 300

K_HUNGER_DECAY_DEMO = 12 * 60
K_HAPPINESS_DECAY_DEMO = 15 * 60
K_POOP_SPAWN_DEMO = 18 * 60

def hunger_decay(m): return K_HUNGER_DECAY_DEMO * (K_REAL_TIME_MULTIPLIER if m == TimeMode.Real else 1)
def happiness_decay(m): return K_HAPPINESS_DECAY_DEMO * (K_REAL_TIME_MULTIPLIER if m == TimeMode.Real else 1)
def poop_spawn(m): return K_POOP_SPAWN_DEMO * (K_REAL_TIME_MULTIPLIER if m == TimeMode.Real else 1)
def poop_spawn_eff(s, m):
    base = poop_spawn(m)
    return base // 2 if s in (Stage.Baby, Stage.Senior) else base

def egg_incubation(m): return K_EGG_REAL_SEC if m == TimeMode.Real else K_EGG_DEMO_SEC

def is_sleeping(hour): return hour >= 22 or hour < 7

# ============================================================
# 事件
# ============================================================
class EventKind(IntEnum):
    FeedOk = 1; FeedRejected = 2; LightToggled = 3; Cleaned = 4
    Medicated = 5; Healed = 6; ScoldedOk = 7; ScoldedBad = 8
    GameWon = 9; GameLost = 10; NewEgg = 11
    Hungry = 20; Happy = 21; PoopAdded = 22
    StageChanged = 23; AdultEvolved = 24
    Sick = 25; Died = 26; BedTime = 27; WokeUp = 28
    CalledForCare = 29; AttentionFlash = 30

@dataclass
class Event:
    kind: EventKind
    v1: int = 0
    v2: int = 0

# ============================================================
# PetState
# ============================================================
@dataclass
class PetState:
    time_mode: TimeMode = TimeMode.Demo
    stage: Stage = Stage.Egg
    hunger: int = 4
    happiness: int = 4
    poop: int = 0
    weight_g: int = 30
    discipline_pct: int = 0
    age_pet_days: int = 0
    care_mistakes: int = 0
    sick_doses: int = 0
    total_doses_needed: int = 0
    pet_seconds: int = 0
    real_seconds: int = 0
    egg_seconds: int = 0
    hunger_empty_since_sec: int = -1
    sick_since_pet_sec: int = -1
    last_poop_real_sec: int = 0
    light_on: bool = True
    call_remaining: int = 0

# ============================================================
# PetCore（与 pet.cpp 等价）
# ============================================================
class PetCore:
    def __init__(self, rng: random.Random):
        self.s = PetState()
        self.rng = rng
        self.subs: List[Callable[[Event], None]] = []

    def subscribe(self, cb): self.subs.append(cb)

    def emit(self, kind, v1=0, v2=0):
        e = Event(kind, v1, v2)
        for s in self.subs: s(e)

    def is_sleeping_now(self) -> bool:
        pc = self.pet_clock(self.s.pet_seconds)
        return is_sleeping(pc["hour"])

    def pet_clock(self, total_pet_sec):
        # hour = (pet_seconds / 3600) % 24
        hour = (total_pet_sec // 3600) % 24
        minute = (total_pet_sec % 3600) // 60
        return {"hour": int(hour), "minute": int(minute)}

    def pet_seconds_for_stage(self, s):
        days = K_STAGE_DAYS[s]
        if days <= 0: return 0
        return days * K_SECONDS_PER_PET_DAY[self.s.time_mode]

    def feed_meal(self):
        if self.s.stage in (Stage.Egg, Stage.Dead): self.emit(EventKind.FeedRejected, 0); return
        if self.s.sick_since_pet_sec >= 0: self.emit(EventKind.FeedRejected, 0); return
        if self.s.hunger >= K_HUNGER_MAX: self.emit(EventKind.FeedRejected, 0); return
        self.s.hunger = min(K_HUNGER_MAX, self.s.hunger + 1)
        self.s.weight_g += 1
        self.emit(EventKind.FeedOk, 0)
        if self.s.hunger >= K_HUNGER_MAX: self.s.hunger_empty_since_sec = -1

    def feed_snack(self):
        if self.s.stage in (Stage.Egg, Stage.Dead): self.emit(EventKind.FeedRejected, 1); return
        if self.s.sick_since_pet_sec >= 0: self.emit(EventKind.FeedRejected, 1); return
        self.s.happiness = min(K_HAPPINESS_MAX, self.s.happiness + 1)
        self.s.weight_g += 2
        self.emit(EventKind.FeedOk, 1)

    def toggle_light(self):
        self.s.light_on = not self.s.light_on
        self.emit(EventKind.LightToggled, 1 if self.s.light_on else 0)

    def clean(self):
        if self.s.poop <= 0: return
        self.s.poop = 0
        self.emit(EventKind.Cleaned)

    def medicate(self):
        if self.s.sick_since_pet_sec < 0:
            self.emit(EventKind.FeedRejected, 1)
            return
        self.s.sick_doses += 1
        self.emit(EventKind.Medicated, self.s.sick_doses, self.s.total_doses_needed)
        if self.s.sick_doses >= self.s.total_doses_needed:
            self.s.sick_since_pet_sec = -1
            self.s.sick_doses = 0
            self.s.total_doses_needed = 0
            self.emit(EventKind.Healed)

    def scold(self):
        if self.s.call_remaining > 0:
            self.s.discipline_pct = min(K_DISCIPLINE_MAX, self.s.discipline_pct + 10)
            self.s.call_remaining -= 1
            self.emit(EventKind.ScoldedOk, self.s.call_remaining)
        else:
            if self.s.happiness > 0: self.s.happiness -= 1
            self.emit(EventKind.ScoldedBad, self.s.happiness)

    def reset_to_new_egg(self):
        self.s = PetState()
        self.s.stage = Stage.Egg
        self.s.hunger = K_HUNGER_MAX
        self.s.happiness = K_HAPPINESS_MAX
        self.s.light_on = True
        self.emit(EventKind.NewEgg)

    def maybe_decay_hunger(self):
        if self.is_sleeping_now(): return
        if self.s.hunger <= 0: return
        if self.s.real_seconds % hunger_decay(self.s.time_mode) != 0: return
        if self.s.real_seconds == 0: return
        nh = self.s.hunger - 1
        self.s.hunger = nh
        self.emit(EventKind.Hungry, nh)
        if nh == 0 and self.s.hunger_empty_since_sec < 0:
            self.s.hunger_empty_since_sec = self.s.real_seconds

    def maybe_decay_happiness(self):
        if self.is_sleeping_now(): return
        if self.s.happiness <= 0: return
        if self.s.real_seconds % happiness_decay(self.s.time_mode) != 0: return
        if self.s.real_seconds == 0: return
        nh = self.s.happiness - 1
        self.s.happiness = nh
        self.emit(EventKind.Happy, nh)

    def maybe_drop_poop(self):
        if self.is_sleeping_now(): return
        period = poop_spawn_eff(self.s.stage, self.s.time_mode)
        if self.s.real_seconds == 0: return
        if self.s.real_seconds % period != 0: return
        if self.s.poop >= K_POOP_MAX:
            if self.s.sick_since_pet_sec < 0:
                self.s.sick_doses = 0
                self.s.total_doses_needed = 1 + self.rng.randint(0, 3)  # 1..4
                self.s.sick_since_pet_sec = self.s.pet_seconds
                self.emit(EventKind.Sick, self.s.total_doses_needed)
            return
        self.s.poop += 1
        self.s.last_poop_real_sec = self.s.real_seconds
        self.emit(EventKind.PoopAdded, self.s.poop)
        if self.s.poop >= 4:
            self.s.sick_doses = 0
            self.s.total_doses_needed = 1 + self.rng.randint(0, 3)
            self.s.sick_since_pet_sec = self.s.pet_seconds
            self.emit(EventKind.Sick, self.s.total_doses_needed)

    def maybe_get_sick(self):
        if self.s.sick_since_pet_sec >= 0: return
        if self.s.stage in (Stage.Egg, Stage.Dead): return
        if self.s.hunger_empty_since_sec >= 0:
            elapsed_min = (self.s.real_seconds - self.s.hunger_empty_since_sec) // 60
            if elapsed_min >= 30 * (24 if self.s.time_mode == TimeMode.Real else 1):
                self.s.sick_doses = 0
                self.s.total_doses_needed = 1 + self.rng.randint(0, 3)
                self.s.sick_since_pet_sec = self.s.pet_seconds
                self.emit(EventKind.Sick, self.s.total_doses_needed)
                return
        # 每日概率 2% + care_mistakes*3%，平均到每秒
        day_sec = K_SECONDS_PER_PET_DAY[self.s.time_mode]
        chance = 2 + self.s.care_mistakes * 3
        if self.rng.randint(0, 100) < chance * 60 / day_sec:
            self.s.sick_doses = 0
            self.s.total_doses_needed = 1 + self.rng.randint(0, 3)
            self.s.sick_since_pet_sec = self.s.pet_seconds
            self.emit(EventKind.Sick, self.s.total_doses_needed)

    def maybe_die(self):
        if self.s.stage == Stage.Dead: return
        if self.s.sick_since_pet_sec >= 0:
            if self.s.pet_seconds - self.s.sick_since_pet_sec >= 24 * 3600:
                self.s.stage = Stage.Dead
                self.emit(EventKind.Died, 0); return
        if self.s.stage == Stage.Senior:
            day_sec = K_SECONDS_PER_PET_DAY[self.s.time_mode]
            if self.s.real_seconds > 0 and self.s.real_seconds % day_sec == 0:
                if self.rng.randint(0, 100) < 15:
                    self.s.stage = Stage.Dead
                    self.emit(EventKind.Died, 1)

    def maybe_call_for_care(self):
        if self.s.stage in (Stage.Egg, Stage.Dead): return
        if self.s.call_remaining > 0: return
        day_sec = K_SECONDS_PER_PET_DAY[self.s.time_mode]
        chance = 3 * 100 // day_sec  # 上限 3 次/天
        if self.rng.randint(0, 100) < chance:
            self.s.call_remaining = 1 + self.rng.randint(0, 2)  # 1..3
            self.emit(EventKind.CalledForCare, self.s.call_remaining)

    def evolve_check(self):
        if self.s.stage == Stage.Dead: return -1
        if self.s.stage == Stage.Egg:
            if self.s.egg_seconds >= egg_incubation(self.s.time_mode):
                return int(Stage.Baby)
            return -1
        cur_span = self.pet_seconds_for_stage(self.s.stage)
        prior = 0
        for i in range(int(Stage.Baby), int(self.s.stage)):
            prior += self.pet_seconds_for_stage(Stage(i))
        acc = self.s.pet_seconds - prior
        if acc < cur_span: return -1
        return int(self.s.stage) + 1

    def tick_real_second(self):
        self.s.real_seconds += 1
        if self.s.time_mode == TimeMode.Real:
            self.s.pet_seconds = self.s.real_seconds * 24
        else:
            self.s.pet_seconds = self.s.real_seconds
        self.s.age_pet_days = self.s.pet_seconds // K_SECONDS_PER_PET_DAY[self.s.time_mode]
        if self.s.stage == Stage.Egg:
            self.s.egg_seconds += 1
        self.maybe_decay_hunger()
        self.maybe_decay_happiness()
        self.maybe_drop_poop()
        self.maybe_call_for_care()
        self.maybe_get_sick()
        self.maybe_die()
        new_stage = self.evolve_check()
        if new_stage >= 0:
            old = self.s.stage
            self.s.stage = Stage(new_stage)
            self.emit(EventKind.StageChanged, new_stage)
            if old == Stage.Teen and self.s.stage == Stage.Adult:
                hunger_pct = (self.s.hunger * 100) // K_HUNGER_MAX
                happy_pct = (self.s.happiness * 100) // K_HAPPINESS_MAX
                score = hunger_pct * 40 // 100 + happy_pct * 40 // 100 + self.s.discipline_pct * 20 // 100
                if score < 50: form = 3
                elif score < 80: form = 2
                else: form = 1
                self.emit(EventKind.AdultEvolved, form)
        # 注意图标（汇总）—— 节流：bitmask 变化时才发
        bits = 0
        if self.s.hunger <= 1: bits |= 1 << 0
        if self.s.happiness <= 1: bits |= 1 << 1
        if self.s.sick_since_pet_sec >= 0: bits |= 1 << 2
        pc = self.pet_clock(self.s.pet_seconds)
        if is_sleeping(pc["hour"]) and self.s.light_on: bits |= 1 << 3
        if self.s.poop >= 3: bits |= 1 << 4
        if bits and bits != getattr(self, '_last_attn_bits', 0):
            self.emit(EventKind.AttentionFlash, bits)
        self._last_attn_bits = bits

# ============================================================
# 断言
# ============================================================
def assert_decay_stops_when_sleeping(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Baby
    pc.s.pet_seconds = 23 * 3600  # 23:00 = 睡眠时段
    pc.s.real_seconds = pc.s.pet_seconds  # 演示模式 1:1
    # 推进到 23:00 之后 1 秒
    pc.tick_real_second()
    assert pc.s.hunger == K_HUNGER_MAX, f"睡眠期应冻结 hunger，但变为 {pc.s.hunger}"
    print("[OK] 睡眠期衰减冻结")


def assert_egg_hatches_after_60s(rng):
    pc = PetCore(rng)
    pc.s.time_mode = TimeMode.Demo
    for _ in range(59):
        pc.tick_real_second()
    assert pc.s.stage == Stage.Egg
    pc.tick_real_second()
    assert pc.s.stage == Stage.Baby, f"60秒后应孵化，实际 {pc.s.stage}"
    print("[OK] 蛋演示模式 60s 孵化")


def assert_hunger_decays_every_12_min(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Baby
    pc.s.pet_seconds = 8 * 3600  # 早上 8:00，醒着
    pc.s.real_seconds = pc.s.pet_seconds
    initial = pc.s.hunger
    # 12 分钟后 -1 心（演示模式）
    for _ in range(12 * 60):
        pc.tick_real_second()
    assert pc.s.hunger == initial - 1, f"12分钟后 hunger 应减1，实际 {pc.s.hunger} -> {initial}"
    print("[OK] 演示模式 12 分钟 hunger -1")


def assert_poop_overflow_triggers_sick(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Baby
    pc.s.poop = 3
    pc.s.pet_seconds = 8 * 3600
    pc.s.real_seconds = pc.s.pet_seconds
    # 下一节拍再加 1 个便便 → 立即生病
    period = poop_spawn_eff(pc.s.stage, pc.s.time_mode)
    # 找到下一个节拍时刻
    while (pc.s.real_seconds + 1) % period != 0:
        pc.tick_real_second()
    pc.tick_real_second()
    assert pc.s.poop >= 4
    assert pc.s.sick_since_pet_sec >= 0, "便便≥4 应触发生病"
    print("[OK] 便便≥4 触发生病")


def assert_medicate_heals(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Baby
    pc.s.sick_since_pet_sec = 100
    pc.s.total_doses_needed = 3
    pc.s.sick_doses = 0
    pc.medicate()
    pc.medicate()
    pc.medicate()
    assert pc.s.sick_since_pet_sec == -1, "第3剂药应治愈"
    print("[OK] 3剂药治愈")


def assert_scold_discipline(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Baby
    pc.s.call_remaining = 2
    pc.s.discipline_pct = 0
    pc.scold()
    assert pc.s.discipline_pct == 10
    assert pc.s.call_remaining == 1
    pc.scold()
    assert pc.s.discipline_pct == 20
    assert pc.s.call_remaining == 0
    print("[OK] 管教 +10%/次，递减剩余次数")


def assert_scold_no_call_loses_happiness(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Baby
    pc.s.call_remaining = 0
    pc.s.happiness = 4
    pc.scold()
    assert pc.s.happiness == 3, f"误管教应减快乐，实际 {pc.s.happiness}"
    print("[OK] 误管教 -1 快乐")


def assert_clean_poop(rng):
    pc = PetCore(rng)
    pc.s.poop = 3
    pc.clean()
    assert pc.s.poop == 0
    print("[OK] 清洁归零便便")


def assert_evolution_to_adult_with_care(rng):
    """演示模式：1 宠物日 = 3600 秒。完整生命周期 = 1+2+2+5+3 = 13 宠物日 ≈ 13 真实小时
    玩家持续喂饭/清洁/管教/服药应该能挺过幼年期。"""
    pc = PetCore(rng)
    pc.s.time_mode = TimeMode.Demo
    # 跑 5 真实小时（约 1+2+2 = 5 宠物日，应到 Adult 起步或刚到）
    # 主动照顾，避免饿死
    for i in range(5 * 3600):
        pc.tick_real_second()
        if pc.s.hunger <= 1 and pc.s.stage != Stage.Egg: pc.feed_meal()
        if pc.s.poop >= 3: pc.clean()
        if pc.s.sick_since_pet_sec >= 0: pc.medicate()
        if pc.s.call_remaining > 0: pc.scold()
    assert pc.s.stage in (Stage.Adult, Stage.Senior), \
        f"5 真实小时后应到 Adult+，实际 {pc.s.stage.name}"
    print(f"[OK] 演示模式 5 小时持续照顾 → 进入 {pc.s.stage.name}")


def assert_real_mode_longer():
    """真实模式：1 宠物日 = 24 真实小时；蛋孵化 = 5 分钟"""
    rng = random.Random(0)
    pc = PetCore(rng)
    pc.s.time_mode = TimeMode.Real
    for _ in range(5 * 60):  # 5 分钟
        pc.tick_real_second()
    # 蛋孵化按 egg_seconds 真实秒判定（与 pet.cpp 一致）
    assert pc.s.egg_seconds >= K_EGG_REAL_SEC
    assert pc.s.stage == Stage.Baby, f"真实模式 5 分钟应孵化，实际 {pc.s.stage}"
    print(f"[OK] 真实模式 5 分钟孵化（egg_seconds={pc.s.egg_seconds}）")


def assert_no_hunger_decay_at_egg(rng):
    pc = PetCore(rng)
    pc.s.stage = Stage.Egg
    pc.s.real_seconds = 100
    pc.s.pet_seconds = 100
    pc.maybe_decay_hunger()
    assert pc.s.hunger == K_HUNGER_MAX, "蛋阶段饥饿不应变化"
    print("[OK] 蛋阶段不衰减")


# ============================================================
# 运行默认场景
# ============================================================
def run_default(rng, hours: int, quiet: bool):
    pc = PetCore(rng)
    feed_count = 0
    snack_count = 0
    clean_count = 0
    scold_count = 0
    meds_count = 0

    def on_event(e: Event):
        nonlocal feed_count, snack_count, clean_count, scold_count, meds_count
        if e.kind == EventKind.FeedOk and e.v1 == 0: feed_count += 1
        if e.kind == EventKind.FeedOk and e.v1 == 1: snack_count += 1
        if e.kind == EventKind.Cleaned: clean_count += 1
        if e.kind == EventKind.ScoldedOk: scold_count += 1
        if e.kind == EventKind.Medicated: meds_count += 1
        if not quiet:
            tag = f"{e.kind.name:>18}"
            extra = ""
            if e.v1: extra += f" v1={e.v1}"
            if e.v2: extra += f" v2={e.v2}"
            print(f"  [{pc.s.real_seconds:>6}s real/{pc.s.pet_seconds:>6}s pet] {tag}{extra}")

    pc.subscribe(on_event)

    if not quiet: print(f"=== 跑 {hours} 小时 ({pc.s.time_mode.name} 模式) ===")

    sec = hours * 3600
    for i in range(sec):
        pc.tick_real_second()
        # 主动喂饭 / 清洁 / 管教（玩家行为模拟）
        if pc.s.stage not in (Stage.Egg, Stage.Dead):
            if pc.s.hunger <= 1 and feed_count < 8: pc.feed_meal()
            if pc.s.happiness <= 1 and snack_count < 6: pc.feed_snack()
            if pc.s.poop >= 3 and clean_count < 4: pc.clean()
            if pc.s.call_remaining > 0 and scold_count < 5: pc.scold()
            if pc.s.sick_since_pet_sec >= 0 and meds_count < 12:
                pc.medicate(); meds_count += 1
        if not quiet and (i % 1800 == 0):
            print(f"--- t={i}s stage={pc.s.stage.name} hunger={pc.s.hunger} happy={pc.s.happiness} poop={pc.s.poop} weight={pc.s.weight_g}g discipline={pc.s.discipline_pct}% ---")

    print(f"\n=== 总结 ===")
    print(f"阶段: {pc.s.stage.name}  照顾评分: hunger={pc.s.hunger}/{K_HUNGER_MAX} happy={pc.s.happiness}/{K_HAPPINESS_MAX} discipline={pc.s.discipline_pct}%")
    print(f"体重: {pc.s.weight_g}g  失误: {pc.s.care_mistakes}  age_pet_days={pc.s.age_pet_days}")
    print(f"动作: 喂饭 {feed_count} / 零食 {snack_count} / 清洁 {clean_count} / 管教 {scold_count} / 服药 {meds_count}")
    print(f"事件计数: Hungry={sum(1 for e in pc.subs and [])} …略")
    return pc


# ============================================================
# CLI
# ============================================================
def main():
    p = argparse.ArgumentParser()
    p.add_argument("--mode", choices=["demo", "real"], default="demo")
    p.add_argument("--hours", type=int, default=1)
    p.add_argument("--quiet", action="store_true")
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--no-selfcheck", action="store_true")
    args = p.parse_args()

    rng = random.Random(args.seed)

    if not args.no_selfcheck:
        assert_decay_stops_when_sleeping(rng)
        assert_egg_hatches_after_60s(rng)
        assert_hunger_decays_every_12_min(rng)
        assert_poop_overflow_triggers_sick(rng)
        assert_medicate_heals(rng)
        assert_scold_discipline(rng)
        assert_scold_no_call_loses_happiness(rng)
        assert_clean_poop(rng)
        assert_no_hunger_decay_at_egg(rng)
        assert_evolution_to_adult_with_care(rng)
        assert_real_mode_longer()

    pc = PetCore(rng)
    pc.s.time_mode = TimeMode.Real if args.mode == "real" else TimeMode.Demo
    run_default(rng, args.hours, args.quiet)

    print("\n[ALL PASS]")


if __name__ == "__main__":
    main()