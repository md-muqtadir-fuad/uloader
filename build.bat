@echo off
rem Build U-loader with MinGW-w64 gcc (no other libraries needed).
rem setlocal: the caller keeps its own folder and variables (CFLAGS, ...)
setlocal
cd /d "%~dp0"
if not defined WINDRES set WINDRES=windres
set CFLAGS=-O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
set CORE=src\util.c src\http.c src\ftp.c src\engine.c src\hls.c src\extract.c src\torrent.c src\task.c
set LIBS=-static -s -lwinhttp -lbcrypt -lws2_32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32

gcc %CFLAGS% src\main.c src\host.c %CORE% -o uloader.exe %LIBS%
if errorlevel 1 goto fail

%WINDRES% src\gui.rc -O coff -o src\gui.res
if errorlevel 1 goto fail
gcc %CFLAGS% -mwindows -municode src\gui.c %CORE% src\gui.res -o uloader-gui.exe %LIBS% -lcomctl32 -lgdi32 -luxtheme -lcomdlg32
if errorlevel 1 goto fail

echo Built uloader.exe and uloader-gui.exe
endlocal
exit /b 0
:fail
echo Build failed.
endlocal
exit /b 1
