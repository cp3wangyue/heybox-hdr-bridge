@echo off
chcp 65001 >nul
title 小黑盒 HDR 屏幕共享修复补丁 - 一键安装
cd /d "%~dp0"
echo.
echo ===================================================
echo   小黑盒 HDR 屏幕共享修复补丁 - 一键安装向导
echo ===================================================
echo.
hdrfix_loader.exe --install
echo.
pause
