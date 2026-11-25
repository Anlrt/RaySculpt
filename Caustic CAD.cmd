@echo off
setlocal
set "app=%~dp0RaySculpt.exe"
if exist "%app%" goto launch
set "app=%~dp0build\native\portable\RaySculpt.exe"
if exist "%app%" goto launch
set "app=%~dp0build\native\src\desktop\Release\raysculpt.exe"
if not exist "%app%" goto missing
for /f "tokens=1,* delims==" %%A in ('findstr /B "CMAKE_PREFIX_PATH:" "%~dp0build\native\CMakeCache.txt"') do set "sdk=%%B"
if defined sdk (
    set "PATH=%sdk%\bin;%PATH%"
    set "QT_PLUGIN_PATH=%sdk%\lib\qt6\plugins"
)
:launch
start "" "%app%" %*
exit /b 0
:missing
echo Build RaySculpt with tools\build_native.ps1 or start RaySculpt.exe from the Windows package.
pause
exit /b 1
