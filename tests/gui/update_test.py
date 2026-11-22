"""U-loader's own update in the window (#11), against a fake GitHub release on a local server.

  1. nothing newer: "Check for updates" says so
  2. a newer release: U-loader asks; Yes downloads the setup in the background, checks its size,
     runs it silently with /RESTARTAPP and closes itself (the setup replaces it)
  3. a setup whose size is not the announced one is never run

The "setup" is tests/tools/fakesetup.c: it installs nothing, it only records how it was started.
Run like the other window tests: a test build (ULOADER_TEST_CLASS) in tests/gui/build/.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest as U  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(HERE, "..", ".."))
EXE = os.path.join(HERE, "build", "uloader-gui.exe")
FIX = os.path.abspath(os.path.join(HERE, "..", "fixture"))
T = os.path.join(HERE, "t3")
DATA, OUT, APP = (os.path.join(T, d) for d in ("data", "out", "app"))
LOG = os.path.join(T, "setup-log.txt")
TRACE = os.path.join(T, "trace.txt")
results = []


def ok(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def message_text(dlg):
    return U.dlg_text(dlg, 0xFFFF) or ""          # a message box's text control


def click_button(dlg, text):
    """Click a message box button the way a person does, until the box is gone."""
    import ctypes
    import ctypes.wintypes as W
    for _ in range(20):
        if not U.user32.IsWindow(dlg):
            return True
        found = []
        proto = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)

        def cb(h, lp):
            if U.class_name(h) == "Button" and U.title(h).replace("&", "") == text:
                found.append(h)
            return True
        U.user32.EnumChildWindows(dlg, proto(cb), 0)
        for b in found:
            U.user32.PostMessageW(b, 0x00F5, 0, 0)                # BM_CLICK
        time.sleep(0.3)
    return not U.user32.IsWindow(dlg)


def main():
    shutil.rmtree(T, ignore_errors=True)
    for d in (DATA, OUT, APP):
        os.makedirs(d)
    with open(os.path.join(DATA, "settings.txt"), "w", encoding="utf-8") as f:
        f.write("dir=%s\nupdates=0\nbrowser_guide=0\n" % OUT)        # no automatic check while the test runs
    # a copy "installed by the setup" (it has an uninstaller next to it): it may update itself
    app = os.path.join(APP, "uloader-gui.exe")
    shutil.copy2(EXE, app)
    open(os.path.join(APP, "unins000.exe"), "wb").close()
    fake = os.path.join(FIX, "fakesetup.exe")
    subprocess.run(["gcc", "-O1", os.path.join(PROJ, "tests", "tools", "fakesetup.c"), "-o", fake], check=True)
    size = os.path.getsize(fake)
    port = U.free_port()
    srv = subprocess.Popen([sys.executable, "-I", os.path.join(PROJ, "tests", "server.py"), str(port), FIX,
                            os.path.join(T, "srv.log")])
    base = "http://127.0.0.1:%d" % port
    time.sleep(0.8)
    procs = []

    # a check also runs "yt-dlp -U": keep any yt-dlp on PATH out of reach
    path = os.pathsep.join(d for d in os.environ.get("PATH", "").split(os.pathsep)
                           if not os.path.isfile(os.path.join(d, "yt-dlp.exe")))

    def start(release):
        p = U.start_gui(app, DATA, env_extra={"ULOADER_UPDATE_URL": base + release, "FAKE_SETUP_LOG": LOG,
                                              "ULOADER_TRACE": TRACE, "PATH": path})
        procs.append(p)
        main = U.find_main()
        time.sleep(0.5)
        return p, main

    try:
        # 1. nothing newer
        p, main = start("/release/1.3.0")
        U.command(main, "CHECKUPDATE")
        dlg = U.find_dialog(p.pid, "U-loader", 20)
        ok("no newer version: says so", dlg and "latest version" in message_text(dlg), repr(dlg and message_text(dlg)))
        ok("the message closes with OK", dlg and click_button(dlg, "OK"))
        U.command(main, "EXIT")
        p.wait(15)

        # 2. a newer release: asked, then downloaded, checked and run silently; U-loader closes
        p, main = start("/release/9.9.9?setup=fakesetup.exe&size=%d" % size)
        U.command(main, "CHECKUPDATE")
        dlg = U.find_dialog(p.pid, "U-loader update", 20)
        ok("newer version: asks first", dlg and "9.9.9 is available" in message_text(dlg), repr(dlg and message_text(dlg)))
        ok("nothing downloaded or run before the answer", not os.path.exists(LOG))
        if dlg:
            click_button(dlg, "Yes")
        try:
            p.wait(40)
        except subprocess.TimeoutExpired:
            pass
        ok("U-loader closes for the update", p.poll() == 0, "exit=%s" % p.poll())
        U.wait_for(lambda: os.path.exists(LOG), 10, what="the setup to run")
        cmd = open(LOG).read() if os.path.exists(LOG) else ""
        ok("the setup runs silently and starts U-loader again",
           "/VERYSILENT" in cmd and "/RESTARTAPP" in cmd and "/TRAY" not in cmd, cmd.strip())

        # 3. the downloaded setup is not the announced size: never run
        os.remove(LOG)
        p, main = start("/release/9.9.9?setup=fakesetup.exe&size=%d" % (size + 1))
        U.command(main, "CHECKUPDATE")
        dlg = U.find_dialog(p.pid, "U-loader update", 20)
        if dlg:
            click_button(dlg, "Yes")
        time.sleep(6)
        ok("a setup of the wrong size is not run", not os.path.exists(LOG) and p.poll() is None)
        U.command(main, "EXIT")
        p.wait(15)
    finally:
        for pr in procs:
            if pr.poll() is None:
                pr.kill()
        srv.kill()
        shutil.rmtree(os.path.join(tempfile.gettempdir(), "U-loader-update"), ignore_errors=True)
    failed = [n for n, c in results if not c]
    print("\n%d/%d checks passed" % (len(results) - len(failed), len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
