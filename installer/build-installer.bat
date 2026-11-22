@echo off
rem Builds the U-loader setup program: installer\out\U-loader-Setup-<version>.exe
rem   1. builds uloader.exe and uloader-gui.exe (build.bat)
rem   2. collects the helper programs into installer\bin\ (each one optional):
rem        yt-dlp.exe  - next to uloader.exe in the project folder, or on PATH
rem        ffmpeg.exe  - on PATH        (winget install Gyan.FFmpeg)
rem        deno.exe    - on PATH        (winget install DenoLand.Deno)
rem      A missing one is left out of the setup's components.
rem   3. compiles uloader.iss with Inno Setup 6 (winget install JRSoftware.InnoSetup)
setlocal
cd /d "%~dp0"

call ..\build.bat
if errorlevel 1 exit /b 1

if not exist bin mkdir bin
if not exist out mkdir out
call :collect yt-dlp.exe "..\yt-dlp.exe"
call :collect ffmpeg.exe ""
call :collect deno.exe ""

rem the licence page: U-loader's licence, then the helper programs' notices
copy /y /b ..\LICENSE + ..\THIRD_PARTY_NOTICES.md out\license.txt >nul

set "ISCC="
for %%P in ("%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe") do (
    if not defined ISCC if exist "%%~P" set "ISCC=%%~P"
)
if not defined ISCC (
    echo Inno Setup 6 was not found. Install it with:  winget install JRSoftware.InnoSetup
    exit /b 1
)
"%ISCC%" /Q uloader.iss
if errorlevel 1 (
    echo Building the setup failed.
    exit /b 1
)
echo.
for %%F in (out\U-loader-Setup-*.exe) do echo Built %%F
exit /b 0

rem :collect NAME PREFERRED-PATH  - copy a helper program into bin\ if it can be found
:collect
if exist "bin\%~1" (
    echo   %~1: already in installer\bin
    exit /b 0
)
if not "%~2"=="" if exist "%~2" (
    copy /y "%~2" "bin\%~1" >nul
    echo   %~1: from %~2
    exit /b 0
)
for /f "delims=" %%W in ('where %~1 2^>nul') do (
    copy /y "%%W" "bin\%~1" >nul
    echo   %~1: from %%W
    exit /b 0
)
echo   %~1: not found - the setup will not include it
exit /b 0
