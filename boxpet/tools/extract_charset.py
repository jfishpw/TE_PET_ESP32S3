#!/usr/bin/env python3
# extract_charset.py - 从 main/ 源码字符串字面量提取全部汉字 + 全角/中文标点
# 写入 tools/font_charset.txt（供 gen_font.ps1 生成 LVGL 字体）
import re, io, os

tools_dir = os.path.dirname(os.path.abspath(__file__))
main_dir = os.path.normpath(os.path.join(tools_dir, '..', 'main'))
out_file = os.path.join(tools_dir, 'font_charset.txt')

def wanted(c):
    # CJK 统一汉字；CJK 标点（，。、！？：；《》「」等）；全角形式（～％等）；省略号
    return ('\u4e00' <= c <= '\u9fff'
            or '\u3000' <= c <= '\u303f'
            or '\uff00' <= c <= '\uffef'
            or c in '\u2026')

chars = set()
pat_str = re.compile(r'"([^"\n]*)"')
for root, _, files in os.walk(main_dir):
    for fn in files:
        if not (fn.endswith('.cpp') or fn.endswith('.h')):
            continue
        path = os.path.join(root, fn)
        try:
            text = io.open(path, 'r', encoding='utf-8', errors='ignore').read()
        except OSError:
            continue
        for m in pat_str.finditer(text):
            for c in m.group(1):
                if wanted(c):
                    chars.add(c)

s = ''.join(sorted(chars))
with io.open(out_file, 'w', encoding='utf-8', newline='\n') as f:
    f.write(s + '\n')
print(f'found {len(chars)} unique chars -> {out_file}')
print(s)
