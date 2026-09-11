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

// ===== v5 多阶段多分支形态表（docs/evolution_design.md）=====
// kform：4 蛋皮 idle + 幼生/成长/成熟/完全体 ×力魔速 idle + 各形态动作帧
//   （happy/eat/zzz/scold/sick_<形态名>）。帧名 = 精灵查找键（find_sprite_by_name）。
// ksenior：老年外观 + 通用兜底帧（dead_grave/wedding/born + 裸名动作帧回退）。
extern const Sprite kform_frames[];   extern const int kform_count;
extern const Sprite ksenior_frames[]; extern const int ksenior_count;

}  // namespace boxpet::sprites
