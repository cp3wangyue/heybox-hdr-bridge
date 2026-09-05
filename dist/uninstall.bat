@echo off
chcp 65001 >nul
title 小黑盒 HDR 屏幕共享修复补丁 - 一键卸载
cd /d "%~dp0"
echo.
echo ===================================================
echo   小黑盒 HDR 屏幕共享修复补丁 - 一键卸载向导
echo ===================================================
echo.
hdrfix_loader.exe --uninstall
echo.
pause
