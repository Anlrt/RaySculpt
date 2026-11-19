@echo off
setlocal
cd /d "%~dp0"
if exist "%~dp0.venv\Scripts\pythonw.exe" set "PATH=%~dp0.venv\Scripts;%PATH%"
where pythonw.exe >nul 2>nul
if errorlevel 1 (
    python "%~dp0tools\cad_desktop.py" %*
    if errorlevel 1 pause
) else (
    start "" pythonw.exe "%~dp0tools\cad_desktop.py" %*
)
