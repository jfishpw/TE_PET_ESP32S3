# gen_font.sh — 用 lv_font_conv 从 Windows simhei.ttf 生成 LVGL 字体
# 输出到 main/ui/ui_font_16.c
# 注意：Windows 用户需在 PowerShell 中跑此脚本（在 Bash 环境可直接运行）

set -e
FONT_DIR="$(cd "$(dirname "$0")" && pwd)"
WIN_FONT="/c/Windows/Fonts/simhei.ttf"
OUT="$FONT_DIR/../main/ui/ui_font_16.c"
CHARSET="$FONT_DIR/font_charset.txt"

if [ ! -f "$WIN_FONT" ]; then
  echo "[!] simhei.ttf not found at $WIN_FONT, please change WIN_FONT path."
  exit 1
fi

# 把多行 charset 合并成单行
SYMS=$(tr -d '\n' < "$CHARSET" | sed 's/  */ /g' | sed 's/ *$//')

echo "[*] Generating LVGL font -> $OUT"
npx --yes lv_font_conv@1.5.3 \
  --no-compress --no-prefilter --bpp 4 --size 16 \
  --font "$WIN_FONT" -r 0x20-0x7F \
  --font "$WIN_FONT" --symbols "$SYMS" \
  --format lvgl \
  --lv-include "lvgl.h" \
  --lv-font-name "ui_font_16" \
  -o "$OUT"
echo "[*] Done"