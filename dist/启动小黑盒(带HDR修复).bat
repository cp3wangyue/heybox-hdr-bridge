@echo off
chcp 65001 >nul
title 启动小黑盒语音 (带 HDR 修复)
cd /d "%~dp0"
hdrfix_loader.exe --launch
