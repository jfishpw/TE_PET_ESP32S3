# gen_font.ps1 - Generate LVGL font (Windows)
# Run from boxpet root: powershell -ExecutionPolicy Bypass -File tools/gen_font.ps1
#
# Notes:
#   1. Do NOT use $args as a variable name (PowerShell automatic variable)
#   2. Read charset as UTF-8 so Chinese symbols are not garbled
#   3. Generated struct font name is ui_font_16_src; a pointer alias
#      ui_font_16 is appended to keep ui_font_16.h interface unchanged
#   4. Keep CRLF line endings in this file (PS 5.1 cannot parse
#      here-strings in LF-only files, so this script avoids them)

$ErrorActionPreference = 'Stop'
$fontDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$winFont = 'C:\Windows\Fonts\simhei.ttf'
$out = Join-Path $fontDir '..\main\ui\ui_font_16.c' | Resolve-Path
$charsetFile = Join-Path $fontDir 'font_charset.txt'

if (-not (Test-Path $winFont)) {
  Write-Host "[!] simhei.ttf not found at $winFont" -ForegroundColor Red
  exit 1
}
if (-not (Test-Path $charsetFile)) {
  Write-Host "[!] charset file not found: $charsetFile" -ForegroundColor Red
  exit 1
}
if (-not (Get-Command npx -ErrorAction SilentlyContinue)) {
  Write-Host "[!] npx not found, install Node.js first (https://nodejs.org)" -ForegroundColor Red
  exit 1
}

# Merge multi-line charset into one line (read as UTF-8)
$syms = (Get-Content $charsetFile -Raw -Encoding UTF8) -replace "`r`n", '' -replace "`n", '' -replace '\s+', ' '
if ($null -eq $syms) { $syms = '' }
$syms = $syms.Trim()
if ($syms.Length -eq 0) {
  Write-Host "[!] charset file is empty: $charsetFile" -ForegroundColor Red
  exit 1
}
Write-Host "[*] symbols: $syms"
Write-Host "[*] Generating LVGL font -> $out"

$npxArgs = @(
  '--no-compress','--no-prefilter','--bpp','4','--size','16',
  '--font', $winFont, '-r', '0x20-0x7F',
  '--font', $winFont, '--symbols', $syms,
  '--format','lvgl',
  '--lv-include','lvgl.h',
  '--lv-font-name','ui_font_16_src',
  '-o', $out
)
npx --yes lv_font_conv@1.5.3 @npxArgs

# Append pointer alias lines (no here-string, LF-safe)
$aliasLines = @(
  '',
  '// Pointer alias: keeps the extern const lv_font_t* ui_font_16 interface',
  'const lv_font_t* ui_font_16 = &ui_font_16_src;'
)
Add-Content -Path $out -Value $aliasLines -Encoding ASCII
Write-Host "[*] Done"
