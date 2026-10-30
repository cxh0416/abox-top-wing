@echo off
setlocal
title Top Flying Wing - CAN Keyboard Control
cd /d "%~dp0"
if exist "D:\Anaconda-3\python.exe" (
  "D:\Anaconda-3\python.exe" -B "%~dp0can_keyboard.py" %*
) else (
  python -B "%~dp0can_keyboard.py" %*
)
pause
