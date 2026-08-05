@echo off
REM grab_screenshot.bat - one-shot Clip-Boy badge screenshot.
REM Auto-detects the badge's COM port and writes a timestamped PNG to shots\
REM (grab_screenshot.py now encodes PNG directly - no BMP is created).
REM Just double-click (with a --test build flashed and the badge plugged in),
REM or run from a terminal. Any extra args pass through to the Python tool, e.g.
REM   grab_screenshot.bat --name menu
REM   grab_screenshot.bat --out-dir C:\caps
setlocal
py -3 "%~dp0scripts\grab_screenshot.py" --out-dir "%~dp0shots" %*
set RC=%ERRORLEVEL%
REM Keep the window open if launched by double-click so the path/error is visible.
REM if "%~1"=="" pause
endlocal & exit /b %RC%
