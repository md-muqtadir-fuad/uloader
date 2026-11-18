"""Browser path end to end: fake browser (kill-on-close job) -> uloader.exe native host
-> GUI started through Explorer with --job -> download. Then a second request goes to the
running window over WM_COPYDATA, and "open" shows it. Uses the real %APPDATA%\\U-loader
(an Explorer-started process gets Explorer's environment); removed afterwards if it was
not there before."""
import json
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest as U  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
E2E = os.path.join(HERE, "e2e")
FIX = os.path.abspath(os.path.join(HERE, "..", "fixture"))
OUT = os.path.join(HERE, "t3", "out")
APPDATA_DIR = os.path.join(os.environ["APPDATA"], "U-loader")
existed = os.path.exists(APPDATA_DIR)
results = []


def ok(name, cond, detail=""):
    results.append(bool(cond))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def browser(*msgs):
    r = subprocess.run([sys.executable, "-I", os.path.join(HERE, "fake_browser.py"), os.path.join(E2E, "uloader.exe")]
                       + [json.dumps(m) for m in msgs], capture_output=True, text=True, timeout=60)
    return [json.loads(x) for x in r.stdout.splitlines() if x.startswith("{")], r


shutil.rmtree(os.path.join(HERE, "t3"), ignore_errors=True)
os.makedirs(OUT)
shutil.copy2(os.path.join(HERE, "build", "uloader-gui.exe"), os.path.join(E2E, "uloader-gui.exe"))
port = U.free_port()
srv = subprocess.Popen([sys.executable, "-I", os.path.join(PROJ, "tests", "server.py"), str(port), FIX,
                        os.path.join(HERE, "t3", "srv.log")])
base = "http://127.0.0.1:%d" % port
time.sleep(0.8)
gui_pid = None
try:
    ok("no U-loader window before the test", not U.user32.FindWindowW(U.CLASS, None))
    replies, r = browser({"action": "ping"},
                         {"action": "download", "url": base + "/r/big30.bin", "dir": OUT, "filename": "via-browser.bin",
                          "connections": "8"})
    ok("native host answered", len(replies) == 2 and replies[1].get("ok"), str(replies) + r.stderr[-200:])
    main = U.find_main(20)
    gui_pid = U.window_pid(main)
    import ctypes
    out = ctypes.create_unicode_buffer(1024)
    h = ctypes.windll.kernel32.OpenProcess(0x1000, False, gui_pid)
    n = ctypes.c_ulong(1024)
    ctypes.windll.kernel32.QueryFullProcessImageNameW(h, 0, out, ctypes.byref(n))
    ctypes.windll.kernel32.CloseHandle(h)
    ok("window started from the e2e folder", out.value.lower().endswith(r"e2e\uloader-gui.exe"), out.value)
    ps = subprocess.run(["powershell", "-NoProfile", "-Command",
                         "$p=Get-CimInstance Win32_Process -Filter 'ProcessId=%d'; (Get-Process -Id $p.ParentProcessId).Name" % gui_pid],
                        capture_output=True, text=True).stdout.strip()
    ok("window was started by Explorer (outside the browser job)", ps.lower() == "explorer", "parent: " + ps)
    time.sleep(2.0)
    ok("window survives after the browser's job is closed", U.user32.IsWindow(main) and U.ping(main) is not None)
    U.wait_for(lambda: os.path.exists(os.path.join(OUT, "via-browser.bin")) and
               not os.path.exists(os.path.join(OUT, "via-browser.bin.ulpart")), 60, what="browser download")
    ok("browser download: SHA-256 matches",
       U.sha256(os.path.join(OUT, "via-browser.bin")) == U.sha256(os.path.join(FIX, "big30.bin")))
    ok("Explorer-started window uses %APPDATA%\\U-loader", os.path.exists(os.path.join(APPDATA_DIR, "downloads.txt")))

    replies, r = browser({"action": "download", "url": base + "/r/big.bin", "dir": OUT, "filename": "second-via-browser.bin"},
                         {"action": "open"})
    ok("second request delivered to the running window", len(replies) == 2 and replies[0].get("via") == "gui"
       and replies[1].get("ok"), str(replies))
    U.wait_for(lambda: os.path.exists(os.path.join(OUT, "second-via-browser.bin")) and
               not os.path.exists(os.path.join(OUT, "second-via-browser.bin.ulpart")), 60, what="second download")
    ok("second download: SHA-256 matches",
       U.sha256(os.path.join(OUT, "second-via-browser.bin")) == U.sha256(os.path.join(FIX, "big.bin")))
    ok("still a single window", U.item_count(main) == 2)
    U.command(main, "EXIT")
    U.wait_for(lambda: not U.user32.IsWindow(main), 15, what="exit")
    ok("exit", True)
finally:
    w = U.user32.FindWindowW(U.CLASS, None)
    if w:
        U.command(w, "EXIT")
        time.sleep(3)
    srv.kill()
    if not existed and os.path.exists(APPDATA_DIR):
        shutil.rmtree(APPDATA_DIR, ignore_errors=True)
        print("removed", APPDATA_DIR)
print("%d/%d checks passed" % (sum(results), len(results)))
