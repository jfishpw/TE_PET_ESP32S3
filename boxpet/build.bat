@echo off
REM build.bat — 一键激活 + 编译 BoxPet（PowerShell 不熟悉的同事用）
REM 用法： build.bat          （编译）
REM       build.bat flash    （烧录）
REM       build.bat monitor  （监视）
REM       build.bat clean    （清理）

setlocal
set IDF_PATH=D:\Espressif\frameworks\esp-idf-v5.4.4
if exist "%IDF_PATH%\export.bat" (
    call "%IDF_PATH%\export.bat" || (echo [!] export.bat failed & exit /b 1)
) else (
    REM v5.4+ 默认只安装 PowerShell 脚本；此处临时用 powershell 引导
    powershell -NoProfile -ExecutionPolicy Bypass -Command "& '%IDF_PATH%\export.ps1'" || (echo [!] export.ps1 failed & exit /b 1)
)
cd /d %~dp0

if "%1"=="" goto build
if /I "%1"=="build"     goto build
if /I "%1"=="flash"     goto flash
if /I "%1"=="monitor"   goto monitor
if /I "%1"=="menuconfig" goto menuconfig
if /I "%1"=="clean"     goto clean
if /I "%1"=="all"       goto all

echo [!] Unknown action: %1
echo     Usage: build.bat [build^|flash^|monitor^|menuconfig^|clean^|all]
exit /b 1

:build
idf.py build
goto end

:flash
idf.py flash
goto end

:monitor
idf.py monitor
goto end

:menuconfig
idf.py menuconfig
goto end

:clean
idf.py fullclean
goto end

:all
idf.py build flash monitor
goto end

:end
endlocal