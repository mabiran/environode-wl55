@echo off
REM EnviroNode-WL55 dashboard - one-time local install (Windows).
setlocal
cd /d "%~dp0"

echo Locating Python...
set PY=
where py >nul 2>nul && set PY=py -3
if "%PY%"=="" ( where python >nul 2>nul && set PY=python )
if "%PY%"=="" (
  echo ERROR: Python 3 not found. Install it from https://www.python.org/downloads/
  echo        and tick "Add python.exe to PATH", then re-run install.bat.
  pause & exit /b 1
)
echo Using: %PY%

echo Creating virtual environment .venv ...
%PY% -m venv .venv || ( echo venv creation failed & pause & exit /b 1 )

echo Installing dependencies ...
call ".venv\Scripts\python.exe" -m pip install --upgrade pip
call ".venv\Scripts\python.exe" -m pip install -r requirements.txt || ( echo pip install failed & pause & exit /b 1 )

echo Running self-test ...
call ".venv\Scripts\python.exe" app.py --selftest || ( echo self-test FAILED & pause & exit /b 1 )

echo.
echo Install complete. Next:
echo   1) copy .streamlit\secrets.toml.example  to  .streamlit\secrets.toml  and fill in your TTN API key
echo   2) double-click run.bat
echo.
pause
