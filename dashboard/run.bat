@echo off
REM EnviroNode-WL55 dashboard - launch (Windows).
setlocal
cd /d "%~dp0"
if not exist ".venv\Scripts\python.exe" (
  echo Virtual environment not found. Run install.bat first.
  pause & exit /b 1
)
echo Starting dashboard at http://localhost:8501  (Ctrl+C to stop)
call ".venv\Scripts\python.exe" -m streamlit run app.py
pause
