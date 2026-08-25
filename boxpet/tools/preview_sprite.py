#!/usr/bin/env python3
# preview_sprite.py - 把 pet_sprites.cpp 数据按 4bpp 解包画 ASCII 图（验证图形）
import io, re, os, sys

W = H = 48
src = io.open(os.path.join(os.path.dirname(__file__), '..', 'main', 'sprites', 'pet_sprites.cpp'),
              encoding='utf-8').read()

# 解析所有 {name, {bytes}} 帧
frames = {}
for m in re.finditer(r'\{"([a-z_]+)",\s*\{(.*?)\}\}', src, re.S):
    name = m.group(1)
    data = [int(b, 16) for b in re.findall(r'0x([0-9A-Fa-f]{2})', m.group(2))]
    frames[name] = data

CH = '.123456789abcdef'

def show(name):
    data = frames[name]
    assert len(data) == W * H // 2, f'{name}: {len(data)} bytes'
    print(f'=== {name} ===')
    for y in range(H):
        row = ''
        for x in range(W):
            b = data[(y * W + x) // 2]
            idx = (b >> 4) if x % 2 == 0 else (b & 0xF)
            row += CH[idx]
        print(row)

for n in sys.argv[1:] if len(sys.argv) > 1 else ['egg', 'baby']:
    show(n)
