@echo off
REM ===========================================================================
REM  KoreroNet — double-click this in Windows Explorer to rebuild BOTH cores
REM  and flash the board. It just calls flash.ps1 -Build with the execution
REM  policy bypassed, and pauses at the end so you can read the result.
REM ===========================================================================
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash.ps1" -Build
echo.
echo ==================  done  ==================
pause
