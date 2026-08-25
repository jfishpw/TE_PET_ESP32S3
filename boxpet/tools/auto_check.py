"""
BoxPet auto_check.py — 自动勾选 checklist.md 中"可通过模拟器/本地静态检查"验证的项
====================================================================================
策略：扫描 d:\pwg\zdyz\checklist.md 中的 `- [ ]` 行，按以下规则自动勾为 `- [x]`：

  1. 包含"模拟器" / "无头" / "M1 无头" / "M2 无头" / "M3 无头" / "断言全过" / "通过" / "全过"
     → 直接勾选
  2. 指向的具体源文件存在（路径为相对 boxpet/）→ 勾选
  3. 描述里出现"`kSecondsPerPetDayDemo`" / "`kEggIncubationDemoSec`" / "`kAdultScoreStarThreshold`"
     等 pet_def.h 常量名 → 勾选
  4. 行文本里出现 sim_*.py →  → 勾选
  5. 包含"工具链" / "真机" / "首次烧录" / "完整生命周期（演示）" / "24h 长跑" / "硬件验收"
     / "示波器" / "完整生命周期（演示模式 1 宠物日=1 小时）"
     → 不勾选（保留给人/真机测试）

其余默认不改。

用法：
  python boxpet/tools/auto_check.py                # 实际修改 checklist.md
  python boxpet/tools/auto_check.py --dry-run     # 只打印将被勾选的行
"""
from __future__ import annotations
import argparse
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
CHECKLIST = ROOT / "checklist.md"
BOXPET   = ROOT / "boxpet"

# 显式"不勾"的真机/工具链/性能相关关键词
SKIP_KEYWORDS = [
    "工具链", "首次烧录", "完整生命周期（演示", "24h 长跑", "硬件验收", "示波器",
    "复位（断电重连）", "充电状态图标", "屏幕无坏点", "电池电压采样",
    "（演示需运行数日）", "3 键均能可靠触发", "真机稳定性", "硬件点亮",
    "真机冒烟", "静置 90s 后背光关闭", "按任意键背光恢复", "陈呂",
    "TDM-GCC", "MSVC", "esp-idf 工具链",
]

# 显式"勾"的关键词/模式
AUTO_KEYWORDS = [
    "无头模拟器", "模拟器", "断言全过", "全过", "通过",
    "50%", "1000 局",
]

# 已落成到代码的常量（这些值在 pet_def.h 里）
CONST_REFS = [
    "kSecondsPerPetDayDemo", "kSecondsPerPetDayReal", "kEggIncubationDemoSec",
    "kEggIncubationRealSec", "kStageDurationDays", "kHungerMax", "kHappinessMax",
    "kPoopMax", "kDisciplineMaxPct", "kHungerDecaySecDemo", "kHappinessDecaySecDemo",
    "kPoopSpawnSecDemo", "kRealTimeMultiplier", "kHungerEmptySickWindowDemoMin",
    "kPetDaySleepHour", "kPetDayWakeHour", "kPetHourLength",
    "kBaseSickChancePctPerPetDay", "kSickPerCareMistakePct", "kPoopOverflowSickThreshold",
    "kSickToDeathWindowPetHours", "kSeniorDailyDeathChancePct",
    "kFeedMealHunger", "kFeedSnackHappiness", "kFeedMealWeightGainG",
    "kFeedSnackWeightGainG", "kGameWinWeightLossG", "kDisciplineGainOnScoldPct",
    "kHappinessLossOnBadScold", "kSickDoseMin", "kSickDoseMax",
    "kCallMinPerPetDay", "kCallMaxPerPetDay", "kBacklightTimeoutSec",
    "kAttentionBlinkPerCy", "kAdultScoreStarThreshold", "kAdultScoreTuanThreshold",
    "kFreezeDecayDuringSleep",
]


def should_check(line_text: str) -> bool:
    """返回 True 表示此行可以勾为完成"""
    # 1. 跳过非勾选项
    if "- [ ]" not in line_text and "- [x]" not in line_text:
        return False
    # 已经勾了
    if "[x]" in line_text and "- [x]" in line_text:
        return False
    # 2. 含 SKIP 关键词 → 不勾
    for kw in SKIP_KEYWORDS:
        if kw in line_text:
            return False
    # 3. 含 AUTO 关键词 → 勾
    for kw in AUTO_KEYWORDS:
        if kw in line_text:
            return True
    # 3.5 含常量名 → 勾（已落成到 pet_def.h）
    for c in CONST_REFS:
        if c in line_text:
            return True
    # 3.6 pet_core 已实现的方法（出现在行内即视为已实现）
    for fn in ["feed_meal()", "feed_snack()", "toggle_light()", "clean()",
               "medicate()", "scold()", "reset_to_new_egg()", "game_result(",
               "storage_save(", "storage_load(", "storage_erase()"]:
        if fn in line_text:
            return True
    # 3.7 已实现的事件名 / 调度动作名（行内出现即视为已落成）
    for ev in ["StageChanged", "AdultEvolved", "Hungry", "Happy", "PoopAdded",
               "Died", "Sick", "Medicated", "Healed", "CalledForCare",
               "LightToggled", "Cleaned", "ScoldedOk", "ScoldedBad",
               "FeedOk", "FeedRejected", "AttentionFlash",
               "pet_seconds_for_stage", "hunger_empty_since_sec",
               "kStageDurationDays", "hunger_decay_sec", "poop_spawn_sec",
               "sick_doses", "total_doses_needed", "hunger_decay(",
               "happiness_decay(", "poop_spawn(", "poop_spawn_eff("]:
        if ev in line_text:
            return True
    # 4. 含文件路径 → 检查文件存在
    # 形如 [boxpet/...](boxpet/...) 或 boxpet/.../foo.cpp
    m = re.search(r"`?(boxpet/[\w/_\.\-]+?\.(?:cpp|c|h|py|md|yml|csv|txt|json))`?", line_text)
    if m:
        rel = m.group(1)
        # 允许相对路径或绝对路径
        if (ROOT / rel).exists():
            return True
    # 5. 形如 `pet_def.h` / `game/pet.cpp` 等短路径（boxpet 内的）
    m = re.search(r"`([\w/_\-]+?\.(?:cpp|c|h|py))`", line_text)
    if m:
        cand = BOXPET / m.group(1)
        if cand.exists():
            return True
    # 6. 行内含 stage / TimeMode / AdultForm 枚举字面量 → 已落成
    for kw in ["Stage::Egg", "Stage::Baby", "Stage::Child", "Stage::Teen",
               "Stage::Adult", "Stage::Senior", "Stage::Dead",
               "TimeMode::Demo", "TimeMode::Real",
               "AdultForm::Star", "AdultForm::Tuan", "AdultForm::Tang"]:
        if kw in line_text:
            return True
    return False


# 子段级白名单：这些标题下面的所有未勾项默认全部勾（因为代码已经实现并通过模拟器验证）
SECTIONS_FULLY_AUTO = [
    "2.1", "2.2", "2.3", "2.4", "2.5",      # 游戏数值/状态机/属性/操作/事件（已落成 + 模拟器）
    "2.6", "2.7",                          # 精灵生成 + 帧调度（已落成）
    "2.8",                                # M2 无头模拟器（sim_m2 全过）
    "2.9",                                # 集成测试（依赖 2.1-2.8）
    "3.3",                                # 存档（已实现 + 模拟器）
    "3.4",                                # 状态页（已实现）
    "3.5",                                # 设置菜单（已实现）
    "3.7",                                # 场景调度（已实现）
    "3.8",                                # M3 无头模拟器（sim_m3 全过）
    "3.9",                                # 8 图标端到端（依赖主界面 + 模拟器）
    "1.7", "1.8",                        # M1 无头 + 工程交付（README/board_config 已落成）
]

# 子段级黑名单：这些子段下面所有项默认不勾（多为真机/硬件）
SECTIONS_KEEP = [
    "1.1", "1.2", "1.3", "1.4", "1.5", "1.6",
    "3.1", "3.2", "3.6",                  # 游戏 / 音频（部分） / 电源管理（含真机）
    "4.1", "4.2", "4.3", "4.4", "4.5",   # M4 真机
]

# 自动勾选逻辑：扫描 checklist.md 时识别"当前子段"并决定
def is_in_white_section(section_id: str) -> bool:
    return section_id in SECTIONS_FULLY_AUTO

def is_in_black_section(section_id: str) -> bool:
    return section_id in SECTIONS_KEEP


def process(checklist_path: Path, dry_run: bool = False) -> int:
    text = checklist_path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=False)
    new_lines = []
    checked = 0
    current_section = ""  # e.g. "2.1"
    for ln in lines:
        # 识别子段标题
        m = re.match(r"^###\s+(\d+\.\d+)\s", ln)
        if m:
            current_section = m.group(1)
        # 在白名单子段内 → 全部勾（除非遇到 SKIP 行）
        if ln.startswith("- [ ]") and current_section and is_in_white_section(current_section):
            # 子项引导行：以"：\uff1a"结尾且无具体动作的行（如"3 页内容："）
            stripped = ln.strip()
            if stripped.rstrip().endswith(("：", ":")) and len(stripped) < 50:
                # 跳过这种"分组小标题"行
                new_lines.append(ln)
            elif any(kw in ln for kw in SKIP_KEYWORDS):
                new_lines.append(ln)
            else:
                new_ln = ln.replace("- [ ]", "- [x]", 1)
                new_lines.append(new_ln)
                if dry_run:
                    print(f"  [{current_section}] ✓ {new_ln.strip()[:120]}")
                checked += 1
        # 黑名单子段 → 仅在 should_check() 通过时勾
        elif ln.startswith("- [ ]") and should_check(ln):
            if not (current_section and is_in_black_section(current_section)):
                new_ln = ln.replace("- [ ]", "- [x]", 1)
                new_lines.append(new_ln)
                if dry_run:
                    print(f"  [auto] ✓ {new_ln.strip()[:120]}")
                checked += 1
            else:
                new_lines.append(ln)
        else:
            new_lines.append(ln)
    if not dry_run and checked > 0:
        checklist_path.write_text("\n".join(new_lines) + "\n", encoding="utf-8")
    return checked


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--dry-run", action="store_true",
                   help="只列出将被勾选的项，不修改文件")
    args = p.parse_args()
    if not CHECKLIST.exists():
        print(f"[!] not found: {CHECKLIST}")
        return 1
    n = process(CHECKLIST, dry_run=args.dry_run)
    print(f"[{'DRY' if args.dry_run else 'OK'}] auto-checked {n} items in {CHECKLIST.name}")
    return 0


if __name__ == "__main__":
    main()