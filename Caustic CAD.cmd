@echo off
setlocal
cd /d "%~dp0"
set "PYTHONUTF8=1"
if exist ".venv\Scripts\python.exe" goto deps
if exist "build\ui-libs\PySide6" if exist "build\cad-libs\OCP" goto local
where py.exe >nul 2>nul
if errorlevel 1 goto usepython
py -3.14 -m venv .venv
if errorlevel 1 goto failed
goto deps
:usepython
python -c "import sys; sys.exit(sys.version_info[:2] != (3, 14) or sys.maxsize < 2**32)"
if errorlevel 1 goto failed
python -m venv .venv
if errorlevel 1 goto failed
:deps
set "PATH=%~dp0.venv\Scripts;%PATH%"
if exist ".venv\requirements-installed.txt" (
  fc /b requirements.txt .venv\requirements-installed.txt >nul
  if not errorlevel 1 goto local
)
".venv\Scripts\python.exe" -m pip install --only-binary=:all: -r requirements.txt
if errorlevel 1 goto failed
copy /y requirements.txt .venv\requirements-installed.txt >nul
:local
start "" pythonw.exe "%~dp0tools\cad_desktop.py" %*
exit /b 0
:failed
echo Install Python 3.14 x64 and check your Internet connection, then start again.
pause
exit /b 1
