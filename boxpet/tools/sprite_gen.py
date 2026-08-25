"""
BoxPet 像素精灵生成器（tools/sprite_gen.py）
===========================================
读取 `sprites/*.txt`（ASCII 画稿，X=前景、.=背景、空格=透明），
生成 `main/sprites/pet_sprites.cpp` 与 `main/sprites/ui_sprites.cpp`，
提供 LVGL 风格的 C 数组（1bpp，每行 4 字节；32x32 sprite → 128B）。

每个 .txt 是一组帧，按行 `# frame NAME` 分隔。例如 egg_0.txt：

    # frame egg_0
    ................................
    .....XXXXXXXXXXXXXXXXXXXXXX.....
    ....X....................X......
    ...

32 行 32 列，'X' 或 'O'/'#' 等非 '.'' ' 都算前景。
"""
import os
import re
import sys
import textwrap
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "sprites"
OUT = ROOT / "main" / "sprites"

SPRITE_W = 32
SPRITE_H = 32

FOREGROUND_CHARS = set("XO#@*ox")


def parse_file(path: Path):
    frames = []
    cur_name = None
    cur_lines = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.rstrip("\n")
        m = re.match(r"\s*#\s*frame\s+([A-Za-z0-9_]+)\s*$", line)
        if m:
            if cur_name is not None:
                frames.append((cur_name, cur_lines))
            cur_name = m.group(1)
            cur_lines = []
            continue
        if cur_name is None:
            continue  # 开头注释
        cur_lines.append(line)
    if cur_name is not None:
        frames.append((cur_name, cur_lines))
    return frames


def ascii_to_bitmap(lines):
    """把 ASCII 32 行转成 1bpp bytes（每行 32 bit = 4 bytes，MSB = 左）"""
    if len(lines) < SPRITE_H:
        # 不足 32 行用空补
        lines = lines + ["" ] * (SPRITE_H - len(lines))
    elif len(lines) > SPRITE_H:
        lines = lines[:SPRITE_H]
    out = bytearray()
    for ln in lines:
        # 补齐 32 列
        if len(ln) < SPRITE_W:
            ln = ln + " " * (SPRITE_W - len(ln))
        bits = 0
        for x in range(SPRITE_W):
            ch = ln[x]
            if ch in FOREGROUND_CHARS:
                bits |= (1 << (SPRITE_W - 1 - x))
        # 4 字节（big-endian：MSB 在 byte[0]）
        out += bytes([(bits >> 24) & 0xFF, (bits >> 16) & 0xFF,
                      (bits >> 8) & 0xFF, bits & 0xFF])
    return bytes(out)


def to_c_array(b: bytes) -> str:
    parts = [f"0x{x:02X}" for x in b]
    return ", ".join(parts)


def gen_one_module(module_name: str, file_list):
    lines = [
        f"// {module_name}.cpp — 自动生成（tools/sprite_gen.py），不要手动改",
        "// 32x32 1bpp 精灵（每行 4 字节，共 128 字节 / 帧）",
        '#include "sprites.h"',
        "",
        "namespace boxpet::sprites {",
        "",
    ]
    for fname, sprite_id, frames in file_list:
        lines.append(f"// ===== {fname} =====")
        lines.append(f"const Sprite k{fname}_frames[] = {{")
        for f_name, f_data in frames:
            bits = ascii_to_bitmap(f_data)
            c_arr = to_c_array(bits)
            lines.append(f"    {{ \"{f_name}\", {{ {c_arr} }} }},")
        lines.append("};")
        lines.append(f"const int k{fname}_count = sizeof(k{fname}_frames)/sizeof(k{fname}_frames[0]);")
        lines.append("")
    lines.append("}  // namespace boxpet::sprites")
    return "\n".join(lines) + "\n"


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    pet_files, ui_files = [], []
    if not SRC.exists():
        SRC.mkdir(parents=True)
        print(f"[i] created {SRC}/ — please drop *.txt frame files here.")
        sys.exit(0)
    for p in sorted(SRC.glob("*.txt")):
        frames = parse_file(p)
        if not frames:
            continue
        stem = p.stem
        if stem.startswith("ui_"):
            ui_files.append((stem, stem[3:], frames))
        else:
            pet_files.append((stem, stem, frames))
    (OUT / "pet_sprites.cpp").write_text(
        gen_one_module("pet_sprites", pet_files), encoding="utf-8")
    (OUT / "ui_sprites.cpp").write_text(
        gen_one_module("ui_sprites", ui_files), encoding="utf-8")
    print(f"[ok] {len(pet_files)} pet sprites, {len(ui_files)} ui sprites → {OUT}")


if __name__ == "__main__":
    main()