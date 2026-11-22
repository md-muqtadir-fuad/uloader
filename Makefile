# U-loader - build with MinGW-w64 gcc:  make        (or run build.bat)
#   uloader.exe       command line + browser bridge
#   uloader-gui.exe   the window
CC      = gcc
WINDRES ?= windres
CFLAGS  = -O2 -Wall -Wextra -Wno-unused-parameter -std=gnu11
LDFLAGS = -static -s
LIBS    = -lwinhttp -lbcrypt -lws2_32 -lshell32 -lole32 -loleaut32 -luuid -ladvapi32
GUILIBS = -lcomctl32 -lgdi32 -luxtheme -lcomdlg32
CORE    = src/util.c src/http.c src/ftp.c src/engine.c src/hls.c src/extract.c \
          src/torrent.c src/task.c
HDRS    = src/uloader.h
GUIRES  = src/gui.rc src/gui_res.h src/gui.manifest src/gui.ico

all: uloader.exe uloader-gui.exe

uloader.exe: src/main.c src/host.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) src/main.c src/host.c $(CORE) -o $@ $(LDFLAGS) $(LIBS)

src/gui.res: $(GUIRES)
	$(WINDRES) src/gui.rc -O coff -o $@

uloader-gui.exe: src/gui.c src/gui_res.h src/gui.res $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -mwindows -municode src/gui.c $(CORE) src/gui.res -o $@ $(LDFLAGS) $(LIBS) $(GUILIBS)

test: uloader.exe
	python -I tests/run_tests.py uloader.exe

# make runs recipes in cmd.exe (mingw32-make alone) or in sh (MSYS, Git
# Bash). cmd.exe echoes the quotes, sh does not: delete with the right tool.
ifeq ($(shell echo "x"),"x")
  RM_FILES = del /q $(subst /,\,$(1)) 2>NUL
else
  RM_FILES = rm -f $(1)
endif

clean:
	-$(call RM_FILES,uloader.exe uloader-gui.exe src/gui.res)

.PHONY: all test clean
