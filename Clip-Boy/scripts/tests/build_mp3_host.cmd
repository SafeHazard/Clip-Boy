@echo off
REM Build + run the streaming-MP3 host proof with MSVC. Artifacts go to %TMPDIR%.
REM Usage: build_mp3_host.cmd <path-to.mp3> [more.mp3 ...]
setlocal
set VCV="C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set ROOT=%~dp0..\..
set OUT=%CLAUDE_JOB_DIR%\tmp
if "%CLAUDE_JOB_DIR%"=="" set OUT=%TEMP%
call %VCV% >nul || (echo vcvars failed & exit /b 1)
cl /nologo /O2 /W3 /I "%ROOT%\libs\minimp3" /I "%ROOT%" ^
   "%ROOT%\scripts\tests\mp3_stream_host.c" ^
   /Fe:"%OUT%\mp3_stream_host.exe" /Fo:"%OUT%\mp3_stream_host.obj" || (echo compile failed & exit /b 1)
echo === compile OK ===
:run
if "%~1"=="" goto done
"%OUT%\mp3_stream_host.exe" "%~1" || (echo TEST FAILED for %~1 & exit /b 1)
shift
goto run
:done
echo === all tests passed ===
endlocal
