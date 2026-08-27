@echo off
setlocal
cd /d "%~dp0"
if exist ".venv\Scripts\python.exe" ( call ".venv\Scripts\python.exe" app.py --selftest ) else ( py -3 app.py --selftest )
pause
