// sprites.h — BoxPet 彩色像素精灵定义（48x48，4bpp 索引色 + 16 色全局调色板）
// 数据定义在 sprites/pet_sprites.cpp（由 tools/sprite_gen2.py 生成/维护）
// 索引 0 = 透明；1..f = kPalette 颜色。
#pragma once

#include <cstdint>

namespace boxpet::sprites {

constexpr int kSpriteWidth  = 48;
constexpr int kSpriteHeight = 48;
constexpr int kSpriteBytes  = (kSpriteWidth * kSpriteHeight) / 2;  // 4bpp 打包 = 1152

struct Sprite {
    const char* name;
    uint8_t     bitmap[kSpriteBytes];   // 高半字节 = 左像素，低半字节 = 右像素
};

// 全局 16 色调色板（RGB888，渲染时转 lv_color_hex）
extern const uint32_t kPalette[16];

// ===== 宠物各阶段（idle 单帧，呼吸动画由渲染偏移实现） =====
extern const Sprite kegg_frames[];         extern const int kegg_count;
extern const Sprite kbaby_frames[];        extern const int kbaby_count;
extern const Sprite kchild_frames[];       extern const int kchild_count;
extern const Sprite kteen_frames[];        extern const int kteen_count;
extern const Sprite kadult_star_frames[];  extern const int kadult_star_count;
extern const Sprite kadult_tuan_frames[];  extern const int kadult_tuan_count;
extern const Sprite kadult_tang_frames[];  extern const int kadult_tang_count;
extern const Sprite ksenior_frames[];      extern const int ksenior_count;

}  // namespace boxpet::sprites
