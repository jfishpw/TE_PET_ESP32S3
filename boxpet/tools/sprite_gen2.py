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

def draw_egg():
    img = blank()
    ellipse_fill(img, 24, 26, 15, 18, CH[4])
    # 蛋壳高光
    for x, y in ((17, 15), (18, 14), (19, 14), (16, 16), (17, 16)):
        px(img, x, y, CH[3])
    # 斑点（深粉）
    for sx, sy, r in ((19, 24, 2), (28, 30, 2), (24, 36, 2), (30, 20, 1)):
        ellipse_fill(img, sx, sy, r, r, CH[5])
    outline(img, CH[1])
    return img

def draw_baby():
    img = blank()
    # 小圆身
    ellipse_fill(img, 24, 26, 13, 14, CH[4])
    # 耳朵
    ellipse_fill(img, 15, 14, 3, 4, CH[4])
    ellipse_fill(img, 33, 14, 3, 4, CH[4])
    ellipse_fill(img, 15, 14, 1, 2, CH[5])
    ellipse_fill(img, 33, 14, 1, 2, CH[5])
    # 肚皮
    ellipse_fill(img, 24, 31, 8, 6, CH[3])
    # 脸
    eyes(img, 24, 22, 6)
    blush(img, 24, 27, 10)
    smile(img, 24, 25, 2)
    # 小脚
    ellipse_fill(img, 19, 41, 3, 2, CH[4])
    ellipse_fill(img, 29, 41, 3, 2, CH[4])
    outline(img, CH[1])
    return img

def draw_child():
    img = blank()
    # 中等身（蓝）
    ellipse_fill(img, 24, 26, 15, 16, CH[6])
    # 头顶呆毛
    px(img, 24, 8, CH[6]); px(img, 24, 9, CH[6]); px(img, 23, 9, CH[6]); px(img, 25, 9, CH[6])
    px(img, 22, 10, CH[6]); px(img, 26, 10, CH[6])
    # 肚皮
    ellipse_fill(img, 24, 32, 10, 7, CH[3])
    # 脸
    eyes(img, 24, 21, 7, r=3)
    blush(img, 24, 27, 12)
    smile(img, 24, 26, 3)
    # 手
    ellipse_fill(img, 8, 28, 3, 4, CH[6])
    ellipse_fill(img, 40, 28, 3, 4, CH[6])
    # 脚
    ellipse_fill(img, 18, 43, 4, 2, CH[6])
    ellipse_fill(img, 30, 43, 4, 2, CH[6])
    outline(img, CH[1])
    return img

def draw_child_bad():
    img = blank()
    # 灰色颓废版儿童：无呆毛、半闭眼、撇嘴、头顶杂草
    ellipse_fill(img, 24, 27, 15, 15, CH[14])
    # 头顶杂草（两根乱毛）
    px(img, 20, 10, CH[13]); px(img, 21, 11, CH[13])
    px(img, 27, 9,  CH[13]); px(img, 28, 10, CH[13]); px(img, 28, 11, CH[13])
    # 肚皮
    ellipse_fill(img, 24, 33, 10, 6, CH[3])
    # 半闭眼
    for ex in (17, 31):
        rect(img, ex - 3, 21, ex + 3, 22, CH[1])
    # 撇嘴
    px(img, 22, 28, CH[1]); px(img, 23, 29, CH[1]); px(img, 24, 28, CH[1])
    px(img, 25, 29, CH[1]); px(img, 26, 28, CH[1])
    # 垂下的手
    ellipse_fill(img, 9, 31, 3, 4, CH[14])
    ellipse_fill(img, 39, 31, 3, 4, CH[14])
    ellipse_fill(img, 18, 43, 4, 2, CH[14])
    ellipse_fill(img, 30, 43, 4, 2, CH[14])
    outline(img, CH[1])
    return img

def draw_teen():
    img = blank()
    # 瘦高身（紫）
    ellipse_fill(img, 24, 25, 13, 18, CH[10])
    # 发型（深紫刘海）
    ellipse_fill(img, 24, 12, 12, 6, CH[11])
    for x in range(12, 37):
        if (x // 2) % 2 == 0:
            for y in range(12, 17):
                if img[y][x] == CH[10]:
                    img[y][x] = CH[11]
    # 肚皮
    ellipse_fill(img, 24, 32, 8, 7, CH[3])
    # 酷眼（半闭）
    for ex in (17, 31):
        rect(img, ex - 3, 20, ex + 3, 22, CH[1])
        rect(img, ex - 2, 21, ex, 22, CH[2])
    # 嘴（一侧挑）
    px(img, 24, 28, CH[1]); px(img, 25, 28, CH[1]); px(img, 26, 27, CH[1])
    # 手
    ellipse_fill(img, 9, 30, 3, 4, CH[10])
    ellipse_fill(img, 39, 30, 3, 4, CH[10])
    # 脚
    ellipse_fill(img, 19, 44, 3, 2, CH[10])
    ellipse_fill(img, 29, 44, 3, 2, CH[10])
    outline(img, CH[1])
    return img

def draw_teen_bad():
    img = blank()
    # 暗灰驼背少年：乱发、黑眼圈、叹气
    ellipse_fill(img, 24, 26, 13, 17, CH[15])
    # 乱发（深灰竖条）
    for hx in (14, 17, 20, 24, 28, 31, 34):
        for hy in range(9, 14):
            if img[hy][hx] == TR:
                px(img, hx, hy, CH[12])
    # 驼背前倾：头身重叠压低
    ellipse_fill(img, 24, 20, 11, 9, CH[15])
    # 黑眼圈
    ellipse_fill(img, 18, 23, 3, 2, CH[14])
    ellipse_fill(img, 30, 23, 3, 2, CH[14])
    # 无神小眼
    px(img, 18, 23, CH[1]); px(img, 30, 23, CH[1])
    # 叹气嘴（O）
    ellipse_fill(img, 24, 28, 2, 2, CH[1])
    # 叹气符号
    px(img, 40, 8, CH[14]); px(img, 41, 8, CH[14])
    px(img, 40, 11, CH[14]); px(img, 41, 11, CH[14])
    px(img, 40, 14, CH[14]); px(img, 41, 14, CH[14])
    # 手（垂）
    ellipse_fill(img, 9, 32, 3, 4, CH[15])
    ellipse_fill(img, 39, 32, 3, 4, CH[15])
    ellipse_fill(img, 19, 44, 3, 2, CH[15])
    ellipse_fill(img, 29, 44, 3, 2, CH[15])
    outline(img, CH[1])
    return img

def draw_adult_star():
    img = blank()
    # 圆身（金）
    ellipse_fill(img, 24, 27, 15, 15, CH[8])
    # 头顶星星
    star = [(24,2),(22,6),(19,6),(21,9),(20,12),(24,10),(28,12),(27,9),(29,6),(26,6)]
    for x, y in star:
        px(img, x, y, CH[8])
        px(img, x, y + 1, CH[9])
    # 肚皮
    ellipse_fill(img, 24, 33, 10, 6, CH[3])
    # 亮眼睛
    eyes(img, 24, 22, 7, r=3)
    blush(img, 24, 27, 12)
    smile(img, 24, 26, 3)
    # 手（张开庆祝）
    ellipse_fill(img, 7, 24, 3, 4, CH[8])
    ellipse_fill(img, 41, 24, 3, 4, CH[8])
    # 脚
    ellipse_fill(img, 18, 43, 4, 2, CH[8])
    ellipse_fill(img, 30, 43, 4, 2, CH[8])
    outline(img, CH[1])
    return img

def draw_adult_tuan():
    img = blank()
    # 圆胖（橙）
    ellipse_fill(img, 24, 26, 17, 17, CH[9])
    # 肚皮大
    ellipse_fill(img, 24, 33, 12, 8, CH[3])
    # 眯眯眼
    for ex in (17, 31):
        rect(img, ex - 3, 21, ex + 3, 22, CH[1])
    blush(img, 24, 26, 12)
    # 嘴（满足）
    smile(img, 24, 26, 2)
    # 短手放肚子上
    ellipse_fill(img, 8, 32, 3, 3, CH[9])
    ellipse_fill(img, 40, 32, 3, 3, CH[9])
    # 脚
    ellipse_fill(img, 17, 44, 4, 2, CH[9])
    ellipse_fill(img, 31, 44, 4, 2, CH[9])
    outline(img, CH[1])
    return img

def draw_adult_tang():
    img = blank()
    # 扁圆躺着（绿）
    ellipse_fill(img, 24, 30, 18, 12, CH[13])
    # 肚皮
    ellipse_fill(img, 24, 34, 11, 6, CH[3])
    # 半闭眼
    for ex in (16, 32):
        rect(img, ex - 3, 24, ex + 3, 25, CH[1])
    # 嘴（打哈欠 O）
    ellipse_fill(img, 24, 30, 2, 2, CH[1])
    # 枕着的胳膊
    ellipse_fill(img, 6, 36, 3, 3, CH[13])
    ellipse_fill(img, 42, 36, 3, 3, CH[13])
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

def draw_eat():
    img = draw_baby()
    # 张大嘴
    ellipse_fill(img, 24, 28, 4, 3, CH[1])
    ellipse_fill(img, 24, 29, 2, 1, CH[5])
    # 饭团（右上）
    tri = [(38,20),(41,15),(44,20)]
    for x, y in tri:
        px(img, x, y, CH[3])
    rect(img, 38, 20, 44, 23, CH[3])
    rect(img, 39, 21, 43, 22, CH[3])
    rect(img, 40, 18, 42, 19, CH[3])
    px(img, 39, 19, CH[3]); px(img, 43, 19, CH[3])
    rect(img, 40, 21, 42, 22, CH[1])  # 海苔
    # 眯眼享受
    return img

def draw_sick():
    img = draw_baby()
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

def draw_scold():
    img = draw_baby()
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

def draw_happy():
    img = blank()
    # 跳起来的 baby（整体上移 4）
    ellipse_fill(img, 24, 22, 13, 14, CH[4])
    ellipse_fill(img, 15, 10, 3, 4, CH[4])
    ellipse_fill(img, 33, 10, 3, 4, CH[4])
    ellipse_fill(img, 24, 27, 8, 6, CH[3])
    # 眯眯笑眼（^ ^）
    for ex in (18, 30):
        px(img, ex - 2, 19, CH[1]); px(img, ex - 1, 18, CH[1]); px(img, ex, 18, CH[1]); px(img, ex + 1, 19, CH[1])
    blush(img, 24, 24, 10)
    # 大笑嘴
    ellipse_fill(img, 24, 24, 3, 2, CH[1])
    # 高举的手
    ellipse_fill(img, 8, 12, 3, 4, CH[4])
    ellipse_fill(img, 40, 12, 3, 4, CH[4])
    # 音符
    px(img, 42, 6, CH[7]); px(img, 43, 6, CH[7]); px(img, 43, 5, CH[7]); px(img, 43, 4, CH[7]); px(img, 44, 3, CH[7])
    px(img, 41, 7, CH[7]); px(img, 40, 7, CH[7])
    # 脚（离地）
    ellipse_fill(img, 19, 39, 3, 2, CH[4])
    ellipse_fill(img, 29, 39, 3, 2, CH[4])
    outline(img, CH[1])
    return img

def draw_zzz():
    img = blank()
    # 趴睡的 baby（扁）
    ellipse_fill(img, 24, 32, 16, 11, CH[4])
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
    'egg': draw_egg,
    'baby': draw_baby,
    'child': draw_child,
    'child_bad': draw_child_bad,
    'teen': draw_teen,
    'teen_bad': draw_teen_bad,
    'adult_star': draw_adult_star,
    'adult_tuan': draw_adult_tuan,
    'adult_tang': draw_adult_tang,
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

TABLES = [
    ('kegg_frames', 'kegg_count', ['egg']),
    ('kbaby_frames', 'kbaby_count', ['baby']),
    ('kchild_frames', 'kchild_count', ['child', 'child_bad']),
    ('kteen_frames', 'kteen_count', ['teen', 'teen_bad']),
    ('kadult_star_frames', 'kadult_star_count', ['adult_star']),
    ('kadult_tuan_frames', 'kadult_tuan_count', ['adult_tuan']),
    ('kadult_tang_frames', 'kadult_tang_count', ['adult_tang']),
    ('ksenior_frames', 'ksenior_count',
     ['senior', 'eat', 'sick', 'scold', 'happy', 'zzz', 'dead_grave', 'wedding', 'born']),
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
