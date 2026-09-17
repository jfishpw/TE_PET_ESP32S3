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
# ===== v6 形态重设计：阶段=体型/比例差异，分支=配色+职业特征 =====
# 阶段（体型递进，头身比差异明显）：
#   幼生：单团子（头身合一、圆滚、大手大脚感）
#   成长：头身分离、四肢细长、职业特征初现
#   成熟：成人比例、职业装备齐备（拳套 / 法师帽 / 围巾）
#   完全：巨体格 + 传奇元素（金装 / 星环长袍 / 风翼闪电）
# 分支（配色/特征强区分）：
#   力量=橙 + 红头带 + 白拳套    魔法=淡紫 + 紫法师帽 + 金星
#   速度=绿 + 红围巾 + 浅蓝风线
# ===== v6 形态重设计：阶段=体型/比例差异，分支=配色+职业特征 =====
# 阶段（体型递进，头身比差异明显）：
#   幼生：单团子（头身合一、圆滚、大手大脚感）
#   成长：头身分离、四肢细长、职业特征初现
#   成熟：成人比例、职业装备齐备（拳套 / 法师帽 / 围巾）
#   完全：巨体格 + 传奇元素（金装 / 星环长袍 / 风翼闪电）
# 分支（配色/特征强区分）：
#   力量=橙 + 红头带 + 白拳套    魔法=淡紫 + 紫法师帽 + 金星
#   速度=绿 + 红围巾 + 浅蓝风线
# ===== v6 形态重设计：阶段=体型/比例差异，分支=配色+职业特征 =====
# 阶段（体型递进，头身比差异明显）：
#   幼生：单团子（头身合一、圆滚、大手大脚感）
#   成长：头身分离、四肢细长、职业特征初现
#   成熟：成人比例、职业装备齐备（拳套 / 法师帽 / 围巾）
#   完全：巨体格 + 传奇元素（金装 / 星环长袍 / 风翼闪电）
# 分支（配色/特征强区分）：
#   力量=橙 + 红头带 + 白拳套    魔法=淡紫 + 紫法师帽 + 金星
#   速度=绿 + 红围巾 + 浅蓝风线
# ===== v6 形态重设计：阶段=体型/比例差异，分支=配色+职业特征 =====
# 阶段（体型递进，头身比差异明显）：
#   幼生：单团子（头身合一、圆滚、大手大脚感）
#   成长：头身分离、四肢细长、职业特征初现
#   成熟：成人比例、职业装备齐备（拳套 / 法师帽 / 围巾）
#   完全：巨体格 + 传奇元素（金装 / 星环长袍 / 风翼闪电）
# 分支（配色/特征强区分）：
#   力量=橙 + 红头带 + 白拳套    魔法=淡紫 + 紫法师帽 + 金星
#   速度=绿 + 红围巾 + 浅蓝风线
BRANCH = {
    'force': dict(body=CH[9],  dark=CH[12], metal=CH[2], accent=CH[5], glow=CH[8]),
    'magic': dict(body=CH[10], dark=CH[11], metal=CH[8], accent=CH[2], glow=CH[6]),
    'speed': dict(body=CH[13], dark=CH[15], metal=CH[6], accent=CH[5], glow=CH[6]),
}
# 阶段几何：hr=头半径 hcy=头心 brx/bry=躯干半径 bcy=躯干心 ahy=手高 fy=脚高
GEO = {
    'growth':   dict(hr=6,  hcy=19, brx=9,  bry=10, bcy=33, ahy=32, fy=43),
    'mature':   dict(hr=7,  hcy=15, brx=11, bry=13, bcy=30, ahy=29, fy=45),
    'ultimate': dict(hr=8,  hcy=12, brx=13, bry=16, bcy=29, ahy=28, fy=46),
}

def _face(img, cx, cy, gap, pose, eye_r=2):
    """脸部（eyes/mouth），pose: idle/happy/eat/zzz/sick/scold"""
    if pose == 'happy':
        for ex in (cx - gap, cx + gap):
            px(img, ex - 2, cy, CH[1]); px(img, ex - 1, cy - 1, CH[1])
            px(img, ex, cy - 1, CH[1]); px(img, ex + 1, cy, CH[1])
        ellipse_fill(img, cx, cy + 4, 3, 2, CH[1])       # 张嘴笑
        return
    if pose == 'eat':
        for ex in (cx - gap, cx + gap):
            rect(img, ex - 2, cy, ex + 1, cy, CH[1])
        ellipse_fill(img, cx, cy + 4, 3, 3, CH[1])
        return
    if pose == 'zzz':
        for ex in (cx - gap, cx + gap):
            rect(img, ex - 2, cy + 1, ex + 2, cy + 1, CH[1])
        smile(img, cx, cy + 4, 2)
        # Zzz 标识（蓝色小 Z，左上方漂浮）
        for zx, zy, w in ((7, 9, 3), (13, 5, 2), (17, 10, 2)):
            rect(img, zx, zy, zx + w, zy, CH[7])
            for i in range(w):
                px(img, zx + w - i, zy + 1 + i, CH[7])
            rect(img, zx, zy + w, zx + w, zy + w, CH[7])
        return
    if pose == 'sick':
        for ex in (cx - gap, cx + gap):
            for i in range(-2, 3):
                px(img, ex + i, cy + i, CH[1]); px(img, ex + i, cy - i, CH[1])
        px(img, cx - 3, cy + 4, CH[1]); px(img, cx - 2, cy + 5, CH[1])
        px(img, cx - 1, cy + 4, CH[1]); px(img, cx, cy + 5, CH[1])
        px(img, cx + 1, cy + 4, CH[1]); px(img, cx + 2, cy + 5, CH[1])
        return
    if pose == 'scold':
        for ex in (cx - gap, cx + gap):
            ellipse_fill(img, ex, cy + 1, 2, 2, CH[1])
            px(img, ex, cy, CH[2])
            px(img, ex, cy + 3, CH[6]); px(img, ex, cy + 4, CH[6])
        px(img, cx - 3, cy + 5, CH[1]); px(img, cx - 2, cy + 4, CH[1])
        px(img, cx - 1, cy + 5, CH[1]); px(img, cx, cy + 4, CH[1])
        px(img, cx + 1, cy + 5, CH[1]); px(img, cx + 2, cy + 4, CH[1])
        return
    eyes(img, cx, cy, gap, r=eye_r)
    if pose == 'idle':
        smile(img, cx, cy + 4, 2)

def draw_form(stage, branch, pose='idle'):
    """统一形态绘制：stage ∈ baby/growth/mature/ultimate；branch ∈ force/magic/speed"""
    b = BRANCH[branch]
    body, dark, metal, accent = b['body'], b['dark'], b['metal'], b['accent']
    img = blank()

    if stage == 'baby':
        # ---- 单团子（头身合一）：圆滚、大眼、短四肢 ----
        ellipse_fill(img, 24, 31, 11, 11, body)
        ellipse_fill(img, 24, 35, 7, 5, CH[3])          # 肚皮
        # 分支特征（雏形）
        if branch == 'magic':                            # 呆毛 + 额星
            px(img, 24, 17, body); px(img, 24, 18, body); px(img, 25, 19, body)
            px(img, 23, 20, metal)
        if branch == 'speed':                            # 尖耳 + 小尾
            ellipse_fill(img, 15, 21, 2, 3, body)
            ellipse_fill(img, 33, 21, 2, 3, body)
            px(img, 35, 35, body); px(img, 36, 36, body); px(img, 36, 37, body)
        if branch == 'force':                            # 小拳（圆手）
            ellipse_fill(img, 12, 34, 4, 4, body)
            ellipse_fill(img, 36, 34, 4, 4, body)
            ellipse_fill(img, 12, 33, 1, 1, CH[2])
            ellipse_fill(img, 36, 33, 1, 1, CH[2])
        else:
            ellipse_fill(img, 12, 34, 3, 3, body)
            ellipse_fill(img, 36, 34, 3, 3, body)
        ellipse_fill(img, 19, 42, 3, 2, body)
        ellipse_fill(img, 29, 42, 3, 2, body)
        _face(img, 24, 28, 5, pose, eye_r=2)
        outline(img, CH[1])
        return img

    G = GEO[stage]
    hr, hcy = G['hr'], G['hcy']
    brx, bry, bcy = G['brx'], G['bry'], G['bcy']
    ahy, fy = G['ahy'], G['fy']
    # 分支体型差异（不只是颜色）：力量=宽扁壮实；魔法=瘦高；速度=修长
    if branch == 'force':
        brx += 2; bry -= 1
    elif branch == 'magic':
        brx -= 1; bry += 1
    else:
        brx -= 1; bry += 2

    # ---- 分支"背后/外围"元素（翅膀：完全体速度型）----
    if branch == 'speed' and stage == 'ultimate':
        for s in (-1, 1):
            bx = 24 + s * 14
            for i in range(5):
                px(img, bx + s * i, 20 + i, metal)
                px(img, bx + s * i, 21 + i, metal)

    # ---- 躯干 + 脖子 + 头（头身分离，避免连成一坨像熊）----
    ellipse_fill(img, 24, bcy, brx, bry, body)
    ellipse_fill(img, 24, bcy + 5, brx - 3, bry - 4, CH[3])   # 肚皮
    rect(img, 24 - 2, hcy + hr - 2, 24 + 2, bcy - bry + 3, body)   # 脖子
    # ---- 四肢（下移成"手臂"；力量=粗壮拳、魔法=细臂、速度=小后掠）----
    hy = bcy + 1
    if branch == 'force':
        ellipse_fill(img, 24 - brx - 2, hy, 3, 4, body)
        ellipse_fill(img, 24 + brx + 2, hy, 3, 4, body)
    elif branch == 'magic':
        ellipse_fill(img, 24 - brx - 2, hy + 1, 2, 3, body)
        ellipse_fill(img, 24 + brx + 2, hy + 1, 2, 3, body)
    else:
        ellipse_fill(img, 24 - brx - 2, hy + 2, 2, 2, body)
        ellipse_fill(img, 24 + brx + 2, hy + 2, 2, 2, body)
    ellipse_fill(img, 19, fy, 3, 2, body)
    ellipse_fill(img, 29, fy, 3, 2, body)
    ellipse_fill(img, 24, hcy, hr, hr, body)

    # ---- 分支身体/装备特征 ----
    if branch == 'force':
        # 大拳（白指节）+ 护腕 + 眉峰/头带 + 金腰带
        ellipse_fill(img, 24 - brx - 2, hy + 1, 4, 4, body)
        ellipse_fill(img, 24 + brx + 2, hy + 1, 4, 4, body)
        ellipse_fill(img, 24 - brx - 2, hy, 1, 1, metal)
        ellipse_fill(img, 24 + brx + 2, hy, 1, 1, metal)
        if stage != 'growth':
            rect(img, 24 - brx - 5, hy - 2, 24 - brx, hy - 2, dark)
            rect(img, 24 + brx, hy - 2, 24 + brx + 5, hy - 2, dark)
        if stage == 'growth':                                  # 成长起戴红头带（分支辨识）
            rect(img, 24 - hr, hcy - hr + 2, 24 + hr, hcy - hr + 2, accent)
        if stage in ('mature', 'ultimate'):
            rect(img, 24 - hr + 1, hcy - 3, 24 - 2, hcy - 3, CH[1])
            rect(img, 24 + 2, hcy - 3, 24 + hr - 1, hcy - 3, CH[1])
            rect(img, 24 - hr, hcy - hr + 2, 24 + hr, hcy - hr + 2, accent)
        if stage == 'ultimate':
            rect(img, 24 - brx + 3, bcy + 7, 24 + brx - 3, bcy + 8, metal)
    elif branch == 'magic':
        # 呆毛 + 额星 + 尖顶法师帽 + 法杖（随阶段升级）
        if stage == 'growth':
            px(img, 24, hcy - hr - 1, body); px(img, 24, hcy - hr - 2, body)
            px(img, 25, hcy - hr - 3, body); px(img, 26, hcy - hr - 4, body)
            px(img, 24, hcy - hr + 1, metal)
        if stage == 'growth':                                   # 成长起戴小尖帽（分支辨识）
            for i in range(4):
                y0 = hcy - hr - 4 + i
                rect(img, 24 - i, y0, 24 + i, y0, dark)
            rect(img, 24 - 5, hcy - hr - 1, 24 + 5, hcy - hr, metal)
        if stage in ('mature', 'ultimate'):
            hat_w = 9 if stage == 'mature' else 10
            for i in range(hat_w):
                y0 = hcy - hr - 8 + i
                rect(img, 24 - i, y0, 24 + i, y0, dark)
            rect(img, 24 - hat_w, hcy - hr - 1, 24 + hat_w, hcy - hr + 1, metal)
            px(img, 24, hcy - hr - 8, metal)
        # 法杖（成长起）：棕色杆 + 顶端金星
        staff_top = hcy - (4 if stage == 'growth' else 6)
        rect(img, 42, staff_top + 3, 43, fy - 3, CH[12])
        if stage == 'growth':
            px(img, 42, staff_top, metal); px(img, 43, staff_top, metal)
            px(img, 42, staff_top + 1, metal)
        else:
            px(img, 41, staff_top, metal); px(img, 43, staff_top, metal)
            px(img, 42, staff_top - 1, CH[2]); px(img, 42, staff_top + 1, metal)
        if stage == 'ultimate':
            # 星环（帽子上方）+ 长袍下摆（悬浮感）
            for sx, sy in ((14, hcy - hr - 13), (24, hcy - hr - 16), (34, hcy - hr - 13)):
                px(img, sx, sy, CH[2]); px(img, sx + 1, sy, metal)
            for y in range(bcy + 7, bcy + 15):
                w = brx + (y - (bcy + 7))
                rect(img, 24 - w, y, 24 + w, y, body)
    else:  # speed
        ellipse_fill(img, 15, hcy - hr + 2, 2, 3, body)
        ellipse_fill(img, 33, hcy - hr + 2, 2, 3, body)
        if stage in ('growth', 'mature', 'ultimate'):
            px(img, 24 + brx + 1, bcy + 4, body); px(img, 24 + brx + 3, bcy + 5, body)
            px(img, 24 + brx + 5, bcy + 5, body); px(img, 24 + brx + 6, bcy + 6, body)
        if stage == 'growth':                                   # 成长起戴小围巾（分支辨识）
            rect(img, 24 - brx + 2, bcy - 4, 24 + brx - 2, bcy - 3, accent)
        if stage in ('mature', 'ultimate'):
            rect(img, 24 - brx + 2, bcy - 4, 24 + brx - 2, bcy - 3, accent)
            px(img, 24 + brx - 1, bcy - 5, accent); px(img, 24 + brx + 2, bcy - 6, accent)
            px(img, 24 + brx + 4, bcy - 7, accent)
        if stage == 'ultimate':     # 闪电纹 + 双围巾尾
            px(img, 24, bcy - 1, CH[8]); px(img, 23, bcy, CH[8])
            px(img, 24, bcy + 1, CH[8]); px(img, 25, bcy + 2, CH[8]); px(img, 24, bcy + 3, CH[8])
            px(img, 24 + brx, bcy - 1, accent); px(img, 24 + brx + 3, bcy - 2, accent)
        px(img, 24 + brx + 3, bcy - 3, metal); px(img, 24 + brx + 5, bcy, metal)
        px(img, 24 + brx + 3, bcy + 3, metal)

    # ---- 脸 ----
    _face(img, 24, hcy + 1, (hr - 2) if hr > 6 else 4, pose, eye_r=2)
    outline(img, CH[1])
    return img

# 形态名 → (stage, branch)
FORMS = [
    ('baby_force',     ('baby',     'force')), ('baby_magic',     ('baby',     'magic')),
    ('baby_speed',     ('baby',     'speed')),
    ('growth_force',   ('growth',   'force')), ('growth_magic',   ('growth',   'magic')),
    ('growth_speed',   ('growth',   'speed')),
    ('mature_force',   ('mature',   'force')), ('mature_magic',   ('mature',   'magic')),
    ('mature_speed',   ('mature',   'speed')),
    ('ultimate_force', ('ultimate', 'force')), ('ultimate_magic', ('ultimate', 'magic')),
    ('ultimate_speed', ('ultimate', 'speed')),
]

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
# ===== v6 帧注册：每个形态的 idle + 5 个动作/表情帧（同一形态剪影，仅脸部/姿态不同）=====
POSES = ['happy', 'eat', 'zzz', 'scold', 'sick']
for _name, (_st, _br) in FORMS:
    FRAMES[_name] = (lambda st=_st, br=_br: draw_form(st, br, 'idle'))
    for _p in POSES:
        FRAMES[f'{_p}_{_name}'] = (lambda st=_st, br=_br, p=_p: draw_form(st, br, p))

def gen_form_actions():
    """senior（沿用外观）的动作帧；蛋皮无动作帧。
    12 个阶段分支形态的动作帧已在上方按"同一剪影 + 表情"注册。"""
    out = {}
    for name, color in [('senior', CH[14])]:
        out[f'happy_{name}'] = lambda c=color: draw_happy(c)
        out[f'eat_{name}']   = lambda c=color: draw_eat(c)
        out[f'zzz_{name}']   = lambda c=color: draw_zzz(c)
        out[f'scold_{name}'] = lambda c=color: draw_scold(c)
        out[f'sick_{name}']  = lambda c=color: draw_sick(c)
    return out
FRAMES.update(gen_form_actions())

TABLES = [
    # 全形态表：16 idle（4 蛋皮 + 12 阶段分支）+ 12×5 动作/表情帧
    ('kform_frames', 'kform_count',
     ['egg0', 'egg1', 'egg2', 'egg3'] +
     [n for n, _ in FORMS] +
     [f'{a}_{n}' for n, _ in FORMS for a in ('happy', 'eat', 'zzz', 'scold', 'sick')]),
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
