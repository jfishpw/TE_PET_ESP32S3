# build.ps1 - one-key activate ESP-IDF and build BoxPet.
# Usage:
#   powershell -ExecutionPolicy Bypass -File build.ps1 [build|flash|monitor|menuconfig|clean|all]
#   powershell -ExecutionPolicy Bypass -File build.ps1 set-target CHIP
#
# Notes: PowerShell 5.1 compatible. Plain ASCII + LF.

$ErrorActionPreference = 'Stop'

$IDF_PATH = 'D:\Espressif\frameworks\esp-idf-v5.4.4'
$InitScript = Join-Path $IDF_PATH 'export.ps1'

if (-not (Test-Path $InitScript)) {
    Write-Host ('[!] Not found: ' + $InitScript) -ForegroundColor Red
    Write-Host 'Edit $IDF_PATH in this script to point to your ESP-IDF install dir.' -ForegroundColor Yellow
    exit 1
}

# 0. Network restricted? Default to China mirror.
#    Set $env:BOX_PET_NO_MIRROR=1 to disable.
if (-not $env:BOX_PET_NO_MIRROR) {
    $env:IDF_COMPONENT_REGISTRY_URL = 'https://components-file.espressif.com.cn'
}

# 0.5 Component manager: OFF (offline). Local components via EXTRA_COMPONENT_DIRS.
#     (Registry access is blocked on this network; xz_ws/opus are local components.)
$env:IDF_COMPONENT_MANAGER = '0'

# 1. Activate ESP-IDF environment
Write-Host ('[*] Loading ESP-IDF environment from ' + $InitScript) -ForegroundColor Cyan
. $InitScript
Write-Host '[OK] ESP-IDF environment loaded' -ForegroundColor Green
Write-Host ('    IDF_COMPONENT_REGISTRY_URL = ' + $env:IDF_COMPONENT_REGISTRY_URL) -ForegroundColor DarkGray

# 2. Switch to project dir
# PS5.1: $MyInvocation.MyCommand.Path may be null when run via -File.
# Use $PSScriptRoot (PS3+) as fallback.
$ScriptDir = $PSScriptRoot
if (-not $ScriptDir) { $ScriptDir = (Get-Location).Path }
Set-Location $ScriptDir
Write-Host ('[*] Working dir: ' + (Get-Location)) -ForegroundColor Cyan

# 2.5 Check local components; if ( absent, run fetch_components.py
$CompDir = Join-Path $ScriptDir 'components'
if (-not (Test-Path (Join-Path $CompDir 'lvgl'))) {
    Write-Host '[!] components not present. Running fetch_components.py...' -ForegroundColor Yellow
    try {
        python (Join-Path $ScriptDir 'tools\fetch_components.py')
    } catch {
        Write-Host '[!] fetch_components.py failed. Check network or run manually.' -ForegroundColor Red
        exit 1
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Host '[!] Component fetch failed (VPN or Gitee mirror required).' -ForegroundColor Red
        Write-Host '    Manual: python tools\fetch_components.py --source gitee' -ForegroundColor Yellow
        exit 1
    }
}

# 3. Parse CLI argument
$Action = 'build'
if ($args.Count -gt 0) { $Action = $args[0] }

switch ($Action) {
    'build'      { idf.py build }
    'flash'      { idf.py flash }
    'monitor'    { idf.py monitor }
    'menuconfig' { idf.py menuconfig }
    'clean'      { idf.py fullclean }
    'all'        { idf.py build flash monitor }
    'set-target' {
        if ($args.Count -lt 2) {
            Write-Host 'Usage: .\build.ps1 set-target CHIP' -ForegroundColor Yellow
            exit 1
        }
        idf.py set-target $args[1]
    }
    default {
        Write-Host ('[!] Unknown action: ' + $Action) -ForegroundColor Yellow
        Write-Host '    Usage: build | flash | monitor | menuconfig | clean | all | set-target CHIP' -ForegroundColor Yellow
        exit 1
    }
}