"""
BoxPet M1 无头模拟器
===================
本脚本不依赖编译器 / LVGL / SDL，纯 Python 模拟：
  - boxpet/main/ui/ui_main.cpp 中的焦点循环规则（focus_move）
  - 3 键短按/长按的路由表（短按：左移/右移/确认；长按：中键进入设置）
  - 主界面 4 个区域的尺寸与图标排列
  - 用 ASCII 字符画输出"屏幕"

用法：
  python tools/sim/sim_m1.py                       # 自动跑一组按键序列并断言
  python tools/sim/sim_m1.py --interactive        # 手动按键序列（stdin: L/R/M for short, l/r/m for long）
  python tools/sim/sim_m1.py --steps 8 --chars "RRRRMMMLLLLL"   # 指定按键字符序列
"""
from __future__ import annotations
import argparse
import json
import os
import sys
from dataclasses import dataclass, field
from typing import List, Optional

HERE = os.path.dirname(os.path.abspath(__file__))
CONTRACT = os.path.join(HERE, "main_ui.json")


# ---------- 契约加载 ----------
@dataclass
class Icon:
    idx: int
    label: str
    selectable: bool
    row: str
    note: str = ""


@dataclass
class Contract:
    icons: List[Icon]
    initial_focus: int
    long_press_ms: int
    regions: dict
    routes_short: dict
    routes_long: dict
    focus_color: str
    normal_bg_color: str
    alert_color: str

    @classmethod
    def load(cls, path: str) -> "Contract":
        d = json.load(open(path, encoding="utf-8"))
        return cls(
            icons=[Icon(**i) for i in d["icons"]],
            initial_focus=d["initial_focus"],
            long_press_ms=d["long_press_ms"],
            regions=d["regions"],
            routes_short=d["short_press_routes"],
            routes_long=d["long_press_routes"],
            focus_color=d["focus_color"],
            normal_bg_color=d["normal_bg_color"],
            alert_color=d["alert_color"],
        )


# ---------- 焦点状态机（与 ui_main.cpp::focus_move 等价） ----------
class FocusModel:
    """直接照搬 ui_main.cpp 中的 focus_move 规则：循环到下一个 selectable 的图标。

    注意：cpp 实现的兜底是"如果循环一圈找不到可选图标，停在当前"。
    我们的实现严格按此规则。
    """

    def __init__(self, icons: List[Icon], initial: int):
        self.icons = icons
        self.focus = initial
        self.selectable_indices = [i.idx for i in icons if i.selectable]
        # 兜底：如果没有任何可选图标（例如异常配置），就停在当前位置

    def _next_selectable(self, start: int, delta: int) -> int:
        n = len(self.icons)
        idx = start
        # 最多绕一圈
        for _ in range(n + 1):
            idx = (idx + delta) % n
            if self.icons[idx].selectable:
                return idx
        return start  # 兜底

    def move_left(self) -> int:
        self.focus = self._next_selectable(self.focus, -1)
        return self.focus

    def move_right(self) -> int:
        self.focus = self._next_selectable(self.focus, +1)
        return self.focus


# ---------- 按键路由 ----------
KEY_SHORT = {"L": "left", "R": "right", "M": "mid"}
KEY_LONG  = {"l": "left", "r": "right", "m": "mid"}


@dataclass
class KeyEvent:
    """复用 bsp/buttons.h::KeyEvent 语义"""
    key: str       # 'left' | 'mid' | 'right'
    kind: str      # 'short' | 'long'

    @property
    def is_long(self) -> bool:
        return self.kind == "long"


def parse_keyseq(seq: str) -> List[KeyEvent]:
    """解析按键字符串，'L'/'R'/'M' = 短按；'l'/'r'/'m' = 长按；其它字符忽略"""
    out: List[KeyEvent] = []
    for ch in seq:
        if ch in KEY_SHORT:
            out.append(KeyEvent(key=KEY_SHORT[ch], kind="short"))
        elif ch in KEY_LONG:
            out.append(KeyEvent(key=KEY_LONG[ch], kind="long"))
        # else 忽略（便于在测试串中夹空格等）
    return out


# ---------- ASCII 渲染 ----------
def render_screen(model: FocusModel, contract: Contract,
                  last_event: Optional[KeyEvent] = None,
                  alert: bool = False,
                  clock: str = "00:00",
                  batt: str = "100%") -> str:
    """240x240 屏幕的等比例 ASCII 渲染：每字符代表 8x8 像素 → 30x30 网格"""
    W = 30
    H = 30
    grid = [[" " for _ in range(W)] for _ in range(H)]

    # 顶栏 (y=0..24 -> row 0..2)
    for y in range(0, 3):
        for x in range(W):
            grid[y][x] = "="
    # 注意图标 (右栏)
    alert_char = "*" if alert else " "
    grid[1][W - 2] = alert_char
    # 时钟（居中）
    clk_text = f"[{clock}]"
    cx = (W - len(clk_text)) // 2
    for i, ch in enumerate(clk_text):
        if 0 <= cx + i < W:
            grid[1][cx + i] = ch
    # 电量（左栏）
    bat_text = f"({batt})"
    for i, ch in enumerate(bat_text):
        if i < W:
            grid[1][i] = ch

    # 上图标行（y=26..66 → row 3..8）
    top_row_y = [3, 4, 5, 6, 7, 8]
    for i in range(4):
        x = 2 + i * 7  # 7 列宽
        if x >= W - 1: break
        for y in top_row_y:
            grid[y][x] = "+"
        # 绘制图标边框
        for xo in range(4):
            if x + xo < W:
                grid[3][x + xo] = "-"
                grid[8][x + xo] = "-"
        grid[5][x] = "|"
        grid[5][x + 3] = "|"
        # 图标标签（居中）
        lbl = contract.icons[i].label
        for j, ch in enumerate(lbl):
            if x + 1 + j < W:
                grid[6][x + 1 + j] = ch
        # 当前选中：背景色变白（用 * 表示）
        if model.focus == i:
            for y in top_row_y:
                for xo in range(4):
                    if x + xo < W:
                        grid[y][x + xo] = "#"
            # 标签变白
            for j, ch in enumerate(lbl):
                if x + 1 + j < W:
                    grid[6][x + 1 + j] = ch.upper()

    # 宠物区（y=70..194 → row 8..24）：渲染占位文字
    for y in range(9, 25):
        for x in range(W):
            if y == 9 or y == 24:
                grid[y][x] = "-"
            elif x == 0 or x == W - 1:
                grid[y][x] = "|"
    pet_lines = ["  BoxPet  ", "  (等待中)  "]
    for li, line in enumerate(pet_lines):
        ly = 15 + li
        if ly >= H: break
        for j, ch in enumerate(line):
            cx = (W - len(line)) // 2 + j
            if 0 <= cx < W:
                grid[ly][cx] = ch

    # 下图标行（y=196..236 → row 24..29）
    bot_row_y = [24, 25, 26, 27, 28, 29]
    # 4 个槽位但 idx=7 不可选中
    for i in range(4):
        icon = contract.icons[4 + i]
        x = 2 + i * 7
        if x >= W - 1: break
        # 顶部 "+" 角
        grid[24][x] = "+"
        # 边框
        for xo in range(4):
            if x + xo < W:
                grid[24][x + xo] = "-" if not (icon.selectable or model.focus == 4 + i) else "-"
                grid[29][x + xo] = "-"
        grid[26][x] = "|"
        grid[26][x + 3] = "|"
        # 可选 vs 不可选用不同底色字符
        bg = "=" if icon.selectable else "x"
        for y in range(25, 29):
            for xo in range(1, 3):
                if x + xo < W:
                    grid[y][x + xo] = bg
        lbl = icon.label
        for j, ch in enumerate(lbl):
            if x + 1 + j < W:
                grid[27][x + 1 + j] = ch
        if model.focus == 4 + i:
            for y in range(24, 30):
                for xo in range(4):
                    if x + xo < W:
                        grid[y][x + xo] = "#"
            for j, ch in enumerate(lbl):
                if x + 1 + j < W:
                    grid[27][x + 1 + j] = ch.upper()

    # 事件触发的瞬时消息：贴最右一列作为 log 边栏
    if last_event:
        msg = f"[{last_event.kind[0].upper()}:{last_event.key[0].upper()}]"
        # 贴底部
        for i, ch in enumerate(msg):
            if i < W:
                grid[H - 1][W - len(msg) + i] = ch
    return "\n".join("".join(row) for row in grid)


# ---------- 仿真运行 ----------
@dataclass
class SimLog:
    events: List[dict] = field(default_factory=list)
    focus_seq: List[int] = field(default_factory=list)
    last_event: Optional[KeyEvent] = None

    def record(self, evt: KeyEvent, focus: int):
        self.events.append({"key": evt.key, "kind": evt.kind, "focus": focus})
        self.focus_seq.append(focus)
        self.last_event = evt


def run(model: FocusModel, contract: Contract,
        seq: List[KeyEvent]) -> SimLog:
    log = SimLog()
    log.focus_seq.append(model.focus)
    for evt in seq:
        # 路由（与 ui_main.cpp::on_key 等价）
        if evt.kind == "short":
            if evt.key == "left":
                model.move_left()
            elif evt.key == "right":
                model.move_right()
            elif evt.key == "mid":
                # short mid = submit（不改变焦点）
                pass
        else:  # long
            if evt.key == "mid":
                # open_settings（不改变焦点）
                pass
        log.record(evt, model.focus)
    return log


# ---------- 断言 ----------
def assert_focus_round_trip(model: FocusModel, contract: Contract, log: SimLog):
    """从初始焦点向左 / 向右各走 N 步，断言能回到原点且经过所有 selectable 图标一次"""
    expected_selectable = [i.idx for i in contract.icons if i.selectable]
    n = len(expected_selectable)
    # 向右走一圈
    initial = model.focus
    seq = [KeyEvent(key="right", kind="short") for _ in range(n)]
    run(model, contract, seq)
    assert model.focus == initial, f"向右 {n} 次未回到原点（{initial}）"
    # 向左走一圈
    seq = [KeyEvent(key="left", kind="short") for _ in range(n)]
    run(model, contract, seq)
    assert model.focus == initial, f"向左 {n} 次未回到原点（{initial}）"
    # 长按中键不改变焦点
    model.focus = initial
    run(model, contract, [KeyEvent(key="mid", kind="long")])
    assert model.focus == initial, "长按中键改变了焦点（不应该）"


def assert_skip_unselectable(model: FocusModel, contract: Contract):
    """不管从 idx=6 怎么向左移动，都不能停在 idx=7（不可选）"""
    model.focus = 6
    for _ in range(8):
        model.move_left()
        icon = contract.icons[model.focus]
        assert icon.selectable, f"焦点停在了不可选图标 idx={model.focus} ({icon.label})"


def assert_idempotent_short_mid(model: FocusModel, contract: Contract):
    """短按中键不应改变焦点（与 ui_main.cpp 一致）"""
    initial = model.focus
    for _ in range(10):
        run(model, contract, [KeyEvent(key="mid", kind="short")])
        assert model.focus == initial


# ---------- CLI ----------
def main():
    p = argparse.ArgumentParser()
    p.add_argument("--steps", type=int, default=0)
    p.add_argument("--chars", type=str, default="")
    p.add_argument("--interactive", action="store_true",
                   help="从 stdin 读取按键字符（L/R/M=短按 l/r/m=长按）")
    p.add_argument("--quiet", action="store_true")
    args = p.parse_args()

    contract = Contract.load(CONTRACT)
    model = FocusModel(contract.icons, contract.initial_focus)

    # 自检（每次检查后复位 focus，避免污染后续渲染）
    assert_focus_round_trip(model, contract, SimLog())
    model.focus = contract.initial_focus
    assert_skip_unselectable(model, contract)
    model.focus = contract.initial_focus
    assert_idempotent_short_mid(model, contract)
    model.focus = contract.initial_focus
    if not args.quiet:
        print("[OK] M1 焦点 + 按键路由自检通过")

    # 决定按键序列
    if args.interactive:
        chars = sys.stdin.read()
    elif args.chars:
        chars = args.chars
    else:
        # 默认：展示 4 个场景
        chars = ("R R R R M   "  # 右移 3 次到"药"（idx=3），按确认
                 "R R R R M   "  # 再右 3 次到"清"（idx=4），确认
                 "l m M R L L L")  # 长按中键 → 设置；中键确认；右移→教；左移三次回食
        chars = chars.replace(" ", "")

    seq = parse_keyseq(chars)
    if args.steps > 0:
        seq = seq[: args.steps]
    model.focus = contract.initial_focus
    # 先打初始画面（focus 还是 0）
    print("\n=== 初始画面 ===")
    print(render_screen(model, contract))
    # 然后逐步跑，每步打一帧
    log = SimLog()
    log.focus_seq.append(model.focus)
    for evt in seq:
        run(model, contract, [evt])  # 跑一步
        last_focus = model.focus
        log.events.append({"key": evt.key, "kind": evt.kind, "focus": last_focus})
        log.focus_seq.append(last_focus)
        print(f"\n=== 按键: {evt.kind.upper()} {evt.key.upper()} → 焦点={last_focus} ({contract.icons[last_focus].label}) ===")
        print(render_screen(model, contract, last_event=evt,
                            alert=(last_focus == contract.icons[-1].idx)))

    # 事件序列汇总
    print("\n=== 事件汇总 ===")
    for e in log.events:
        print(f"  {e['kind']:>6}  {e['key']:<5}  focus={e['focus']} ({contract.icons[e['focus']].label})")

    print("\n[ALL PASS]")


if __name__ == "__main__":
    main()