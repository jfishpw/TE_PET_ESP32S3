#!/usr/bin/env python3
# sprite_gen2.py — 生成 48x48 4bpp 彩色宠物精灵（输出 main/sprites/pet_sprites.cpp）
# 用法: python tools/sprite_gen2.py
import os

W = H = 48

PALETTE = [
    0x000000,  # 0 透明（占位）
    0x4A3728,  # 1 轮廓深棕
    0xFFFFFF,  # 2 白
    0xFFF6E3,  # 3 米白（肚皮/蛋壳高光）
    0xF9A8C9,  # 4 粉
    0xEC5F8E,  # 5 深粉/腮红
    0x8ED6F0,  # 6 天蓝
    0x2E86C1,  # 7 深蓝
    0xFFD966,  # 8 金黄
    0xF5A623,  # 9 橙
    0xC9A7EB,  # a 淡紫
    0x7D5BA6,  # b 深紫
    0xA9744F,  # c 棕
    0x9BC53D,  # d 草绿
    0xC9D1D9,  # e 浅灰
    0x6B7B8C,  # f 深灰
]

CH = {i: format(i, 'x') for i in range(16)}
TR = '.'

def blank():
    return [[TR] * W for _ in range(H)]

def px(img, x, y, c):
    if 0 <= x < W and 0 <= y < H:
        img[y][x] = c

def ellipse_fill(img, cx, cy, rx, ry, fill):
    for y in range(H):
        for x in range(W):
            dx = (x - cx) / rx
            dy = (y - cy) / ry
            if dx * dx + dy * dy <= 1.0:
                img[y][x] = fill

def rect(img, x0, y0, x1, y1, fill):
    for y in range(max(0, y0), min(H, y1 + 1)):
        for x in range(max(0, x0), min(W, x1 + 1)):
            img[y][x] = fill

def outline(img, color, cond=None):
    """给当前非透明区域的边界描色。cond(y,x) 可选限制。"""
    edges = []
    for y in range(H):
        for x in range(W):
            if img[y][x] == TR:
                continue
            if cond and not cond(y, x):
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if not (0 <= nx < W and 0 <= ny < H) or img[ny][nx] == TR:
                    edges.append((x, y))
                    break
    for x, y in edges:
        img[y][x] = color

def eyes(img, cx, cy, gap, r=2, shine=True):
    """两只圆眼（轮廓色）+ 白高光。cx/cy 眼中心，gap 两眼间距一半。"""
    for ex in (cx - gap, cx + gap):
        ellipse_fill(img, ex, cy, r, r + 1, CH[1])
        if shine:
            px(img, ex, cy - 1, CH[2])
            px(img, ex + 1, cy - 1, CH[2])

def blush(img, cx, cy, gap):
    for ex in (cx - gap, cx + gap):
        ellipse_fill(img, ex, cy, 3, 2, CH[5])

def smile(img, cx, cy, w=3):
    x0 = cx - w
    x1 = cx + w
    for i, x in enumerate(range(x0, x1 + 1)):
        dy = 0 if (x == x0 or x == x1) else 1
        px(img, x, cy + dy, CH[1])
        if 0 < i < 2 * w:
            px(img, x, cy + dy, CH[1])

def shadow(img, cx, cy, rx):
    for x in range(cx - rx, cx + rx + 1):
        dy = 2 if abs(x - cx) < rx // 2 else 1
        for yy in range(cy, cy + dy):
            px(img, x, yy, CH[0 + 15])  # f 深灰影
    # 用半透明感：把影子上移一行画细点
def pack(img):
    """48x48 字符图 -> 4bpp 打包字节串。"""
    data = bytearray()
    for y in range(H):
        row = img[y]
        for x in range(0, W, 2):
            hi = int(row[x], 16) if row[x] != TR else 0
            lo = int(row[x + 1], 16) if row[x + 1] != TR else 0
            data.append((hi << 4) | lo)
    return bytes(data)

# ============ 各帧绘制 ============

def draw_egg(kind):
    """蛋皮 4 种（新蛋随机 1/4，v5）：0 斑点 / 1 云纹 / 2 星纹 / 3 翠叶。"""
    img = blank()
    if kind == 1:       # 云纹蛋：淡蓝底 + 白云纹
        ellipse_fill(img, 24, 26, 15, 18, CH[6])
        for cx, cy in ((20, 19), (28, 27), (22, 36)):
            ellipse_fill(img, cx, cy, 3, 2, CH[2])
    elif kind == 2:     # 星纹蛋：中蓝底 + 金色小星
        ellipse_fill(img, 24, 26, 15, 18, CH[7])
        for sx, sy in ((18, 16), (28, 22), (21, 32), (30, 36)):
            px(img, sx, sy, CH[8]); px(img, sx + 1, sy + 1, CH[8])
            px(img, sx - 1, sy + 1, CH[8]); px(img, sx, sy + 2, CH[8])
    elif kind == 3:     # 翠叶蛋：米白底 + 绿叶脉
        ellipse_fill(img, 24, 26, 15, 18, CH[3])
        for ly in range(14, 38, 4):
            px(img, 24, ly, CH[13])
            px(img, 21, ly + 1, CH[13]); px(img, 27, ly + 1, CH[13])
    else:               # 斑点蛋（默认）：米白底 + 棕色斑点
        ellipse_fill(img, 24, 26, 15, 18, CH[3])
        for sx, sy, r in ((19, 24, 2), (28, 30, 2), (24, 36, 2), (30, 20, 1)):
            ellipse_fill(img, sx, sy, r, r, CH[12])
    # 蛋壳高光
    for x, y in ((17, 15), (18, 14), (19, 14), (16, 16), (17, 16)):
        px(img, x, y, CH[2])
    outline(img, CH[1])
    return img

# ===== v5 多阶段多分支形态（docs/evolution_design.md）=====
# 每阶段 3 分支（力=橙/魔=金/速=绿），体型随阶段增大、特征逐步丰富：
#   幼生：圆滚小身 + 分支雏形（粗手/呆毛/尖耳）
#   成长：+护腕 / 长钩呆毛+星痕 / 尾巴
#   成熟：+拳套 / 星冠+法杖 / 围巾
#   完全：+金拳套+腰带 / 星环+长袍 / 闪电纹+双围巾
FORCE_C = CH[9]
MAGIC_C = CH[8]
SPEED_C = CH[13]

def _limbs(img, c, hand_y, foot_y, hw=3, hx=13):
    ellipse_fill(img, 24 - hx, hand_y, hw, 3, c)
    ellipse_fill(img, 24 + hx, hand_y, hw, 3, c)
    ellipse_fill(img, 19, foot_y, 3, 2, c)
    ellipse_fill(img, 29, foot_y, 3, 2, c)

# --- 幼生期（LV1，体型最小）---
def draw_baby_force():
    img = blank()
    ellipse_fill(img, 24, 28, 12, 12, FORCE_C)
    ellipse_fill(img, 24, 33, 7, 5, CH[3])
    eyes(img, 24, 24, 5, r=2)
    blush(img, 24, 28, 9)
    smile(img, 24, 27, 2)
    _limbs(img, FORCE_C, 31, 42)
    outline(img, CH[1])
    return img

def draw_baby_magic():
    img = blank()
    ellipse_fill(img, 24, 28, 11, 12, MAGIC_C)
    # 头顶闪电呆毛（魔法雏形）
    px(img, 24, 10, MAGIC_C); px(img, 24, 11, MAGIC_C); px(img, 25, 12, MAGIC_C)
    px(img, 24, 13, MAGIC_C); px(img, 25, 14, MAGIC_C)
    ellipse_fill(img, 24, 33, 7, 5, CH[3])
    eyes(img, 24, 24, 5, r=2)
    blush(img, 24, 28, 9)
    smile(img, 24, 27, 2)
    _limbs(img, MAGIC_C, 31, 42)
    outline(img, CH[1])
    return img

def draw_baby_speed():
    img = blank()
    ellipse_fill(img, 24, 28, 11, 12, SPEED_C)
    # 尖耳（敏捷雏形）
    ellipse_fill(img, 15, 17, 2, 4, SPEED_C)
    ellipse_fill(img, 33, 17, 2, 4, SPEED_C)
    ellipse_fill(img, 24, 33, 7, 5, CH[3])
    eyes(img, 24, 24, 5, r=2)
    blush(img, 24, 28, 9)
    smile(img, 24, 27, 2)
    _limbs(img, SPEED_C, 31, 42)
    # 身侧风线
    px(img, 39, 26, CH[14]); px(img, 40, 28, CH[14]); px(img, 39, 30, CH[14])
    outline(img, CH[1])
    return img

# --- 成长期（LV2）---
def draw_growth_force():
    img = blank()
    ellipse_fill(img, 24, 27, 14, 13, FORCE_C)
    ellipse_fill(img, 24, 33, 9, 6, CH[3])
    # 刚毅眉
    rect(img, 16, 19, 20, 19, CH[1]); rect(img, 28, 19, 32, 19, CH[1])
    eyes(img, 24, 23, 6, r=2)
    smile(img, 24, 27, 2)
    # 粗臂 + 深色护腕（成长期标志）
    ellipse_fill(img, 8, 30, 4, 4, FORCE_C)
    ellipse_fill(img, 40, 30, 4, 4, FORCE_C)
    rect(img, 5, 30, 10, 31, CH[12])
    rect(img, 38, 30, 43, 31, CH[12])
    ellipse_fill(img, 18, 43, 4, 2, FORCE_C)
    ellipse_fill(img, 30, 43, 4, 2, FORCE_C)
    outline(img, CH[1])
    return img

def draw_growth_magic():
    img = blank()
    ellipse_fill(img, 24, 27, 12, 14, MAGIC_C)
    # 长钩呆毛
    px(img, 24, 8, MAGIC_C); px(img, 24, 9, MAGIC_C); px(img, 25, 10, MAGIC_C)
    px(img, 26, 11, MAGIC_C); px(img, 26, 12, MAGIC_C)
    ellipse_fill(img, 24, 33, 8, 6, CH[3])
    eyes(img, 24, 23, 6, r=2)
    blush(img, 24, 28, 10)
    smile(img, 24, 27, 2)
    # 眼下星痕（魔法印记）
    px(img, 17, 28, CH[2]); px(img, 31, 28, CH[2])
    _limbs(img, MAGIC_C, 30, 43)
    outline(img, CH[1])
    return img

def draw_growth_speed():
    img = blank()
    ellipse_fill(img, 24, 27, 12, 15, SPEED_C)   # 拉长身形
    ellipse_fill(img, 24, 34, 7, 6, CH[3])
    eyes(img, 24, 22, 6, r=2)
    smile(img, 24, 27, 2)
    # 尾巴（右下卷起）
    px(img, 36, 36, SPEED_C); px(img, 37, 37, SPEED_C); px(img, 38, 38, SPEED_C)
    px(img, 39, 38, SPEED_C); px(img, 40, 37, SPEED_C)
    _limbs(img, SPEED_C, 30, 44)
    # 风线更密
    px(img, 41, 22, CH[14]); px(img, 42, 25, CH[14])
    px(img, 41, 28, CH[14]); px(img, 42, 31, CH[14])
    outline(img, CH[1])
    return img

# --- 成熟期（LV3）---
def draw_mature_force():
    img = blank()
    ellipse_fill(img, 24, 27, 15, 14, FORCE_C)
    ellipse_fill(img, 24, 34, 10, 6, CH[3])
    rect(img, 15, 18, 20, 18, CH[1]); rect(img, 28, 18, 33, 18, CH[1])
    eyes(img, 24, 22, 7, r=2)
    smile(img, 24, 26, 3)
    # 拳套（白扣）
    ellipse_fill(img, 6, 30, 5, 5, FORCE_C)
    ellipse_fill(img, 42, 30, 5, 5, FORCE_C)
    ellipse_fill(img, 6, 29, 2, 2, CH[2])
    ellipse_fill(img, 42, 29, 2, 2, CH[2])
    ellipse_fill(img, 17, 44, 4, 2, FORCE_C)
    ellipse_fill(img, 31, 44, 4, 2, FORCE_C)
    outline(img, CH[1])
    return img

def draw_mature_magic():
    img = blank()
    ellipse_fill(img, 24, 27, 13, 15, MAGIC_C)
    # 星冠（三芒）
    for sx, sy in ((24, 6), (18, 9), (30, 9)):
        px(img, sx, sy, MAGIC_C); px(img, sx, sy + 1, CH[9])
    ellipse_fill(img, 24, 34, 8, 6, CH[3])
    eyes(img, 24, 22, 7, r=2)
    blush(img, 24, 28, 11)
    smile(img, 24, 27, 3)
    # 法杖（右手持，杖头星光）
    rect(img, 42, 20, 43, 40, CH[12])
    px(img, 42, 17, CH[8]); px(img, 44, 17, CH[8])
    px(img, 43, 16, CH[9]); px(img, 43, 18, CH[9])
    ellipse_fill(img, 10, 30, 3, 4, MAGIC_C)
    ellipse_fill(img, 18, 44, 3, 2, MAGIC_C)
    ellipse_fill(img, 30, 44, 3, 2, MAGIC_C)
    outline(img, CH[1])
    return img

def draw_mature_speed():
    img = blank()
    ellipse_fill(img, 24, 28, 13, 14, SPEED_C)
    ellipse_fill(img, 24, 34, 8, 5, CH[3])
    eyes(img, 24, 23, 7, r=2)
    smile(img, 24, 27, 2)
    # 围巾（颈间红巾向右飘）
    rect(img, 16, 32, 32, 33, CH[5])
    px(img, 33, 32, CH[5]); px(img, 35, 31, CH[5]); px(img, 37, 30, CH[5])
    _limbs(img, SPEED_C, 31, 44)
    # 拖影风线
    px(img, 40, 22, CH[14]); px(img, 41, 26, CH[14])
    px(img, 40, 30, CH[14]); px(img, 41, 34, CH[14])
    outline(img, CH[1])
    return img

# --- 完全体（LV4/5，最大体型 + 传奇装饰）---
def draw_ultimate_force():
    img = blank()
    ellipse_fill(img, 24, 26, 16, 15, FORCE_C)
    ellipse_fill(img, 24, 33, 11, 7, CH[3])
    rect(img, 14, 17, 20, 17, CH[1]); rect(img, 28, 17, 34, 17, CH[1])
    eyes(img, 24, 21, 7, r=3)
    smile(img, 24, 26, 3)
    # 金拳套 ×2
    ellipse_fill(img, 5, 29, 5, 5, FORCE_C)
    ellipse_fill(img, 43, 29, 5, 5, FORCE_C)
    ellipse_fill(img, 5, 28, 2, 2, CH[8])
    ellipse_fill(img, 43, 28, 2, 2, CH[8])
    # 金腰带
    rect(img, 13, 38, 35, 39, CH[8])
    px(img, 22, 38, CH[12]); px(img, 26, 38, CH[12])
    ellipse_fill(img, 16, 45, 4, 2, FORCE_C)
    ellipse_fill(img, 32, 45, 4, 2, FORCE_C)
    outline(img, CH[1])
    return img

def draw_ultimate_magic():
    img = blank()
    ellipse_fill(img, 24, 25, 14, 14, MAGIC_C)
    # 长袍摆（下身展开）
    for y in range(39, 46):
        w = 14 + (y - 39) * 2
        rect(img, 24 - w // 2, y, 24 + w // 2, y, MAGIC_C)
    ellipse_fill(img, 24, 31, 9, 6, CH[3])
    eyes(img, 24, 21, 7, r=3)
    blush(img, 24, 27, 11)
    smile(img, 24, 26, 3)
    # 旋转星环（头顶）
    for sx, sy in ((12, 10), (24, 5), (36, 10)):
        px(img, sx, sy, CH[2]); px(img, sx + 1, sy, CH[8])
    px(img, 16, 7, CH[8]); px(img, 32, 7, CH[8])
    px(img, 24, 9, MAGIC_C); px(img, 23, 10, MAGIC_C); px(img, 25, 10, MAGIC_C)
    # 加长法杖
    rect(img, 43, 16, 44, 42, CH[12])
    px(img, 43, 13, CH[8]); px(img, 45, 13, CH[8])
    px(img, 44, 12, CH[2]); px(img, 44, 14, CH[9])
    ellipse_fill(img, 8, 28, 3, 4, MAGIC_C)
    ellipse_fill(img, 18, 45, 3, 2, MAGIC_C)
    ellipse_fill(img, 30, 45, 3, 2, MAGIC_C)
    outline(img, CH[1])
    return img

def draw_ultimate_speed():
    img = blank()
    ellipse_fill(img, 24, 27, 13, 15, SPEED_C)
    ellipse_fill(img, 24, 33, 8, 6, CH[3])
    eyes(img, 24, 22, 7, r=3)
    smile(img, 24, 26, 2)
    # 身上闪电纹（金黄折线）
    px(img, 24, 16, CH[8]); px(img, 23, 17, CH[8]); px(img, 24, 18, CH[8])
    px(img, 25, 19, CH[8]); px(img, 24, 20, CH[8])
    # 双围巾（红巾向右狂舞）
    rect(img, 15, 30, 32, 31, CH[5])
    px(img, 33, 30, CH[5]); px(img, 36, 28, CH[5]); px(img, 39, 27, CH[5])
    px(img, 33, 31, CH[5]); px(img, 36, 33, CH[5]); px(img, 39, 35, CH[5])
    ellipse_fill(img, 8, 30, 3, 4, SPEED_C)
    ellipse_fill(img, 40, 30, 3, 4, SPEED_C)
    ellipse_fill(img, 17, 45, 4, 2, SPEED_C)
    ellipse_fill(img, 31, 45, 4, 2, SPEED_C)
    # 双侧风线
    px(img, 6, 22, CH[14]); px(img, 5, 26, CH[14])
    px(img, 42, 22, CH[14]); px(img, 43, 26, CH[14])
    outline(img, CH[1])
    return img

def draw_senior():
    img = blank()
    # 圆身（浅灰）
    ellipse_fill(img, 24, 26, 14, 15, CH[14])
    # 白眉毛
    rect(img, 15, 17, 21, 18, CH[2])
    rect(img, 27, 17, 33, 18, CH[2])
    # 眼
    eyes(img, 24, 22, 7, r=2)
    # 白胡子
    ellipse_fill(img, 24, 31, 7, 5, CH[2])
    px(img, 24, 27, CH[1]); px(img, 23, 28, CH[1]); px(img, 25, 28, CH[1])
    # 拐杖
    rect(img, 40, 18, 41, 43, CH[12])
    rect(img, 37, 16, 41, 17, CH[12])
    # 脚
    ellipse_fill(img, 19, 42, 3, 2, CH[14])
    ellipse_fill(img, 29, 42, 3, 2, CH[14])
    outline(img, CH[1])
    return img

def draw_eat(body_color=CH[4]):
    img = blank()
    # 身体基色参数化：不同阶段身体色不同（黄/橙/蓝/紫/绿/灰/粉）
    ellipse_fill(img, 24, 26, 13, 14, body_color)
    # 耳朵
    ellipse_fill(img, 15, 14, 3, 4, body_color)
    ellipse_fill(img, 33, 14, 3, 4, body_color)
    ellipse_fill(img, 15, 14, 1, 2, CH[5])
    ellipse_fill(img, 33, 14, 1, 2, CH[5])
    # 肚皮
    ellipse_fill(img, 24, 31, 8, 6, CH[3])
    # 眯眼享受
    for ex in (18, 30):
        px(img, ex - 2, 21, CH[1]); px(img, ex - 1, 20, CH[1]); px(img, ex, 20, CH[1]); px(img, ex + 1, 21, CH[1])
    # 张大嘴
    ellipse_fill(img, 24, 28, 4, 3, CH[1])
    ellipse_fill(img, 24, 29, 2, 1, CH[5])
    # 小脚
    ellipse_fill(img, 19, 41, 3, 2, body_color)
    ellipse_fill(img, 29, 41, 3, 2, body_color)
    # 饭团（右上）
    tri = [(38,20),(41,15),(44,20)]
    for x, y in tri:
        px(img, x, y, CH[3])
    rect(img, 38, 20, 44, 23, CH[3])
    rect(img, 39, 21, 43, 22, CH[3])
    rect(img, 40, 18, 42, 19, CH[3])
    px(img, 39, 19, CH[3]); px(img, 43, 19, CH[3])
    rect(img, 40, 21, 42, 22, CH[1])  # 海苔
    outline(img, CH[1])
    return img

def _base_baby(body_color):
    """小圆身基座（供 sick/scold 等参数化动作帧叠加表情用，v5 保留）。"""
    img = blank()
    ellipse_fill(img, 24, 26, 13, 14, body_color)
    # 耳朵
    ellipse_fill(img, 15, 14, 3, 4, body_color)
    ellipse_fill(img, 33, 14, 3, 4, body_color)
    ellipse_fill(img, 15, 14, 1, 2, CH[5])
    ellipse_fill(img, 33, 14, 1, 2, CH[5])
    # 肚皮
    ellipse_fill(img, 24, 31, 8, 6, CH[3])
    # 小脚
    ellipse_fill(img, 19, 41, 3, 2, body_color)
    ellipse_fill(img, 29, 41, 3, 2, body_color)
    outline(img, CH[1])
    return img

def draw_sick(body_color=CH[4]):
    img = _base_baby(body_color)
    # 病容：X 眼 + 冷汗 + 温度计
    for ex in (18, 30):
        for i in range(-2, 3):
            px(img, ex + i, 21 + i, CH[1])
            px(img, ex + i, 23 - i - 2 + 2, CH[1])
    # 汗滴
    px(img, 35, 16, CH[6]); px(img, 36, 17, CH[6]); px(img, 35, 18, CH[6])
    # 温度计
    rect(img, 8, 26, 9, 38, CH[2])
    ellipse_fill(img, 8, 40, 2, 2, CH[5])
    # 嘴（波浪）
    px(img, 22, 29, CH[1]); px(img, 23, 30, CH[1]); px(img, 24, 29, CH[1]); px(img, 25, 30, CH[1]); px(img, 26, 29, CH[1])
    return img

def draw_scold(body_color=CH[4]):
    img = _base_baby(body_color)
    # 低头含泪
    eyes_low = 25
    for ex in (18, 30):
        ellipse_fill(img, ex, eyes_low, 2, 3, CH[1])
        px(img, ex, eyes_low - 1, CH[2])
    # 泪滴
    px(img, 18, 29, CH[6]); px(img, 18, 30, CH[6]); px(img, 30, 29, CH[6]); px(img, 30, 30, CH[6])
    # 撇嘴
    px(img, 22, 32, CH[1]); px(img, 23, 31, CH[1]); px(img, 24, 32, CH[1]); px(img, 25, 31, CH[1]); px(img, 26, 32, CH[1])
    # 头顶怒气符号
    px(img, 10, 8, CH[5]); px(img, 11, 9, CH[5]); px(img, 9, 10, CH[5]); px(img, 12, 11, CH[5]); px(img, 10, 12, CH[5])
    return img

def draw_happy(body_color=CH[4]):
    img = blank()
    # 跳起来的 baby（整体上移 4）；body_color 参数化匹配当前 stage 基色
    ellipse_fill(img, 24, 22, 13, 14, body_color)
    ellipse_fill(img, 15, 10, 3, 4, body_color)
    ellipse_fill(img, 33, 10, 3, 4, body_color)
    ellipse_fill(img, 24, 27, 8, 6, CH[3])
    # 眯眯笑眼（^ ^）
    for ex in (18, 30):
        px(img, ex - 2, 19, CH[1]); px(img, ex - 1, 18, CH[1]); px(img, ex, 18, CH[1]); px(img, ex + 1, 19, CH[1])
    blush(img, 24, 24, 10)
    # 大笑嘴
    ellipse_fill(img, 24, 24, 3, 2, CH[1])
    # 高举的手
    ellipse_fill(img, 8, 12, 3, 4, body_color)
    ellipse_fill(img, 40, 12, 3, 4, body_color)
    # 音符
    px(img, 42, 6, CH[7]); px(img, 43, 6, CH[7]); px(img, 43, 5, CH[7]); px(img, 43, 4, CH[7]); px(img, 44, 3, CH[7])
    px(img, 41, 7, CH[7]); px(img, 40, 7, CH[7])
    # 脚（离地）
    ellipse_fill(img, 19, 39, 3, 2, body_color)
    ellipse_fill(img, 29, 39, 3, 2, body_color)
    outline(img, CH[1])
    return img

def draw_zzz(body_color=CH[4]):
    img = blank()
    # 趴睡的身体（扁）；body_color 参数化匹配当前 stage 基色
    ellipse_fill(img, 24, 32, 16, 11, body_color)
    ellipse_fill(img, 24, 36, 10, 5, CH[3])
    # 闭眼
    rect(img, 15, 29, 20, 29, CH[1])
    rect(img, 28, 29, 33, 29, CH[1])
    # Zzz
    zz = [(6,12),(7,12),(8,12),(6,13),(7,14),(8,15),(6,16),(7,16),(8,16)]
    for x, y in zz: px(img, x, y, CH[7])
    zz2 = [(11,7),(12,7),(13,7),(11,8),(12,9),(13,10),(11,11),(12,11),(13,11)]
    for x, y in zz2: px(img, x, y, CH[7])
    px(img, 16, 4, CH[7]); px(img, 17, 4, CH[7]); px(img, 16, 5, CH[7]); px(img, 17, 6, CH[7]); px(img, 16, 7, CH[7]); px(img, 17, 7, CH[7])
    outline(img, CH[1])
    return img

def draw_dead_grave():
    img = blank()
    # 墓碑
    ellipse_fill(img, 24, 26, 12, 8, CH[14])       # 顶部圆弧
    rect(img, 12, 26, 36, 40, CH[14])
    rect(img, 10, 40, 38, 44, CH[15])
    # 墓碑描边内陷 + RIP 横线
    rect(img, 18, 24, 30, 25, CH[15])
    px(img, 22, 30, CH[15]); px(img, 26, 30, CH[15])
    rect(img, 20, 32, 28, 32, CH[15])
    # 小花
    ellipse_fill(img, 36, 38, 2, 2, CH[4])
    px(img, 36, 36, CH[2])
    rect(img, 36, 40, 36, 42, CH[13])
    # 草
    for gx in (14, 18, 30, 34):
        px(img, gx, 43, CH[13]); px(img, gx + 1, 44, CH[13])
    outline(img, CH[1])
    return img

def draw_wedding():
    img = blank()
    # 婚礼：两只宠物并排 + 大爱心 + 头纱/领结
    # 左宠（粉，戴头纱）
    ellipse_fill(img, 13, 30, 9, 10, CH[4])
    # 头纱（白）
    ellipse_fill(img, 13, 21, 6, 3, CH[2])
    px(img, 7, 22, CH[2]); px(img, 6, 23, CH[2]); px(img, 5, 24, CH[2])
    # 右宠（蓝，戴领结）
    ellipse_fill(img, 35, 30, 9, 10, CH[6])
    rect(img, 33, 39, 37, 40, CH[7])   # 领结横
    px(img, 35, 39, CH[9])             # 领结心
    # 两宠的眼睛（幸福眯眯眼 ^ ^）
    for cx in (10, 16, 32, 38):
        px(img, cx - 1, 28, CH[1]); px(img, cx, 27, CH[1]); px(img, cx + 1, 28, CH[1])
    # 腮红
    px(img, 8, 31, CH[5]); px(img, 18, 31, CH[5])
    px(img, 30, 31, CH[5]); px(img, 40, 31, CH[5])
    # 中间大爱心（深粉 + 粉高光）
    heart = [
        (20,10),(21,9),(22,9),(23,10),
        (25,10),(26,9),(27,9),(28,10),
        (19,11),(24,11),(29,11),
        (20,12),(23,12),(24,12),(25,12),(28,12),
        (21,13),(22,13),(26,13),(27,13),
        (22,14),(23,14),(25,14),(26,14),
        (23,15),(24,15),(25,15),
        (24,16),
    ]
    for x, y in heart:
        px(img, x, y, CH[5])
    px(img, 21, 10, CH[4])  # 爱心高光
    # 地面小花瓣
    px(img, 5, 42, CH[8]); px(img, 15, 43, CH[8]); px(img, 31, 43, CH[8]); px(img, 42, 42, CH[8])
    outline(img, CH[1])
    return img

def draw_born():
    img = blank()
    # 宝宝出生：小蛋破壳 + 探出粉色宝宝头 + 闪光星
    ellipse_fill(img, 24, 32, 13, 11, CH[3])
    # 蛋壳裂纹（上半锯齿）
    for i, x in enumerate(range(12, 37, 3)):
        px(img, x, 27 - (i % 2), CH[1])
    # 宝宝头（粉）探出
    ellipse_fill(img, 24, 22, 8, 7, CH[4])
    # 呆毛
    px(img, 24, 13, CH[4]); px(img, 23, 14, CH[4]); px(img, 25, 14, CH[4])
    # 亮眼睛
    eyes(img, 24, 21, 4, r=2)
    # 小嘴
    px(img, 24, 25, CH[1])
    # 腮红
    px(img, 19, 23, CH[5]); px(img, 29, 23, CH[5])
    # 闪光星（金黄）
    for sx, sy in ((6,10),(40,12),(10,38),(38,36)):
        px(img, sx, sy, CH[8]); px(img, sx+1, sy, CH[8])
        px(img, sx, sy+1, CH[8])
    outline(img, CH[1])
    return img

# ============ 输出 ============

FRAMES = {
    'senior': draw_senior,
    'eat': draw_eat,
    'sick': draw_sick,
    'scold': draw_scold,
    'happy': draw_happy,
    'zzz': draw_zzz,
    'dead_grave': draw_dead_grave,
    'wedding': draw_wedding,
    'born': draw_born,
}

# ===== v5 帧注册 =====
FRAMES['egg0'] = lambda: draw_egg(0)
FRAMES['egg1'] = lambda: draw_egg(1)
FRAMES['egg2'] = lambda: draw_egg(2)
FRAMES['egg3'] = lambda: draw_egg(3)
FRAMES['baby_force']     = draw_baby_force
FRAMES['baby_magic']     = draw_baby_magic
FRAMES['baby_speed']     = draw_baby_speed
FRAMES['growth_force']   = draw_growth_force
FRAMES['growth_magic']   = draw_growth_magic
FRAMES['growth_speed']   = draw_growth_speed
FRAMES['mature_force']   = draw_mature_force
FRAMES['mature_magic']   = draw_mature_magic
FRAMES['mature_speed']   = draw_mature_speed
FRAMES['ultimate_force'] = draw_ultimate_force
FRAMES['ultimate_magic'] = draw_ultimate_magic
FRAMES['ultimate_speed'] = draw_ultimate_speed

# 12 个阶段分支形态（idle 帧 + 5 个动作帧姿势模板换色复用）
FORMS = [
    ('baby_force',     FORCE_C), ('baby_magic',     MAGIC_C), ('baby_speed',     SPEED_C),
    ('growth_force',   FORCE_C), ('growth_magic',   MAGIC_C), ('growth_speed',   SPEED_C),
    ('mature_force',   FORCE_C), ('mature_magic',   MAGIC_C), ('mature_speed',   SPEED_C),
    ('ultimate_force', FORCE_C), ('ultimate_magic', MAGIC_C), ('ultimate_speed', SPEED_C),
]

def gen_form_actions():
    """为每个形态生成 happy/eat/zzz/scold/sick 动作帧（姿势模板 + 形态基色）。
    沿用 M12 的颜色一致性方案：动作帧保持形态体色，游戏失败反馈（scold）、
    DEPRESSED（scold）、SICK（sick）长显时不与 idle 帧跳色。
    senior（沿用外观）也生成专属动作帧；蛋皮无动作帧。"""
    out = {}
    for name, color in FORMS + [('senior', CH[14])]:
        out[f'happy_{name}'] = lambda c=color: draw_happy(c)
        out[f'eat_{name}']   = lambda c=color: draw_eat(c)
        out[f'zzz_{name}']   = lambda c=color: draw_zzz(c)
        out[f'scold_{name}'] = lambda c=color: draw_scold(c)
        out[f'sick_{name}']  = lambda c=color: draw_sick(c)
    return out
FRAMES.update(gen_form_actions())

TABLES = [
    # v5 全形态表：16 idle（4 蛋皮 + 12 阶段分支）+ 12×5 动作帧
    ('kform_frames', 'kform_count',
     ['egg0', 'egg1', 'egg2', 'egg3'] +
     [n for n, _ in FORMS] +
     [f'{a}_{n}' for n, _ in FORMS for a in ('happy', 'eat', 'zzz', 'scold', 'sick')]),
    # senior 表：老年外观 + 通用兜底帧（蛋期等无专属动作帧的查询回退）
    ('ksenior_frames', 'ksenior_count',
     ['senior', 'sick', 'scold', 'dead_grave', 'wedding', 'born',
      'eat', 'happy', 'zzz',
      'happy_senior', 'eat_senior', 'zzz_senior', 'scold_senior', 'sick_senior']),
]

def c_bytes(data):
    return ',\n    '.join(
        ','.join(f'0x{b:02X}' for b in data[i:i + 12])
        for i in range(0, len(data), 12)
    )

def main():
    out_dir = os.path.join(os.path.dirname(__file__), '..', 'main', 'sprites')
    lines = []
    lines.append('// pet_sprites.cpp — 48x48 4bpp 彩色宠物精灵（由 tools/sprite_gen2.py 生成，勿手改）')
    lines.append('#include "sprites.h"')
    lines.append('')
    lines.append('namespace boxpet::sprites {')
    lines.append('')
    lines.append('const uint32_t kPalette[16] = {')
    lines.append('    ' + ', '.join(f'0x{c:06X}' for c in PALETTE) + ',')
    lines.append('};')
    for tbl, cnt, names in TABLES:
        lines.append('')
        lines.append(f'const Sprite {tbl}[] = {{')
        for n in names:
            data = pack(FRAMES[n]())
            lines.append(f'    {{"{n}",')
            lines.append('     {' + c_bytes(data) + '}},')
        lines.append('};')
        lines.append(f'const int {cnt} = {len(names)};')
    lines.append('')
    lines.append('}  // namespace boxpet::sprites')
    path = os.path.join(out_dir, 'pet_sprites.cpp')
    with open(path, 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'written {path} ({len(FRAMES)} frames)')

if __name__ == '__main__':
    main()
