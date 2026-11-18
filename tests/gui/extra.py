"""More GUI checks: context menu, delete with file, Del key, tray icon, settings apply, exit with a dialog open."""
import ctypes
import ctypes.wintypes as W
import os
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest as U  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXE = os.path.join(HERE, "build", "uloader-gui.exe")
FIX = os.path.abspath(os.path.join(HERE, "..", "fixture"))
T = os.path.join(HERE, "t2")
DATA, OUT, JOBS, SHOTS = (os.path.join(T, d) for d in ("data", "out", "jobs", "shots"))
TRACE = os.path.join(T, "trace.txt")
results = []


def ok(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def rec(name):
    return next((r for r in U.read_list(DATA) if r.get("out_name") == name), None)


class NOTIFYICONIDENTIFIER(ctypes.Structure):
    _fields_ = [("cbSize", W.DWORD), ("hWnd", W.HWND), ("uID", W.UINT), ("guidItem", ctypes.c_byte * 16)]


def icon_hr(main, uid):
    nii = NOTIFYICONIDENTIFIER(ctypes.sizeof(NOTIFYICONIDENTIFIER), main, uid)
    r = W.RECT()
    return ctypes.windll.shell32.Shell_NotifyIconGetRect(ctypes.byref(nii), ctypes.byref(r)) & 0xFFFFFFFF


def tray_icon_exists(main):
    """S_OK = visible, S_FALSE = in the hidden overflow area; a missing icon gives an error."""
    hr = icon_hr(main, 1)
    tray_icon_exists.hr = (hr, icon_hr(main, 99))
    return hr in (0, 1) and tray_icon_exists.hr[1] >= 0x80000000


def popup_menus(pid):
    out = []
    proto = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, U.LPARAM)

    def cb(h, lp):
        if U.window_pid(h) == pid and U.class_name(h) == "#32768" and U.user32.IsWindowVisible(h):
            out.append(h)
        return True

    U.user32.EnumWindows(proto(cb), 0)
    return out


def main():
    shutil.rmtree(T, ignore_errors=True)
    for d in (DATA, OUT, JOBS, SHOTS):
        os.makedirs(d)
    with open(os.path.join(DATA, "settings.txt"), "w", encoding="utf-8") as f:
        f.write("dir=%s\n" % OUT)
    port = U.free_port()
    srv = subprocess.Popen([sys.executable, "-I", os.path.join(PROJ, "tests", "server.py"), str(port), FIX,
                            os.path.join(T, "srv.log")])
    base = "http://127.0.0.1:%d" % port
    time.sleep(0.8)
    p = U.start_gui(EXE, DATA, env_extra={"ULOADER_TRACE": TRACE})
    try:
        main = U.find_main()
        time.sleep(0.5)
        ok("tray icon present", tray_icon_exists(main), "HRESULT ours 0x%08X, unknown id 0x%08X" % tray_icon_exists.hr)
        lv = U.listview(main)

        # a finished one and two slow ones
        U.send_job(main, U.write_job(os.path.join(JOBS, "a.txt"), url=base + "/r/big.bin", dir=OUT, name="done.bin"))
        U.wait_for(lambda: (rec("done.bin") or {}).get("state") == "done", 30, what="done.bin")
        U.send_job(main, U.write_job(os.path.join(JOBS, "b.txt"), url=base + "/r/big30.bin?kbps=150", dir=OUT,
                                     name="slow1.bin", connections=4))
        U.send_job(main, U.write_job(os.path.join(JOBS, "c.txt"), url=base + "/r/big30.bin?kbps=150", dir=OUT,
                                     name="slow2.bin", connections=4))
        time.sleep(3.0)

        # context menu on the selected row (keyboard style), screenshot, then close it
        U.user32.PostMessageW(main, 0x007B, lv, -1)          # WM_CONTEXTMENU, lParam = -1
        menus = U.wait_for(lambda: popup_menus(p.pid), 5, what="context menu")
        time.sleep(0.4)
        U.capture(menus[0], os.path.join(SHOTS, "context_menu.png"))
        U.capture(main, os.path.join(SHOTS, "main_with_menu.png"))
        U.user32.PostMessageW(menus[0], U.WM_KEYDOWN, 0x1B, 0)   # Esc
        U.wait_for(lambda: not popup_menus(p.pid), 5, what="menu closed")
        ok("context menu opens and closes", True)

        # pause slow2 (selected), then delete it with its file: partial files must go
        U.command(main, "PAUSE")
        U.wait_for(lambda: (rec("slow2.bin") or {}).get("state") == "paused", 20, what="slow2 paused")
        part = os.path.join(OUT, "slow2.bin.ulpart")
        ok("paused download has partial files", os.path.exists(part))
        n0 = U.item_count(main)
        U.command(main, "DELETEFILE")
        box = U.find_dialog(p.pid, "U-loader", 5)
        U.capture(box, os.path.join(SHOTS, "confirm_delete.png"))
        U.user32.PostMessageW(box, U.WM_COMMAND, 1, 0)       # IDOK
        U.wait_for(lambda: U.item_count(main) == n0 - 1, 5, what="row removed")
        U.wait_for(lambda: not os.path.exists(part) and not os.path.exists(part[:-7] + ".ulstate"), 5,
                   what="partials removed")
        ok("delete with file removes the row and the partial data", True)

        # Del key on the list removes the finished one but keeps its file
        U.key(lv, U.VK_HOME)
        time.sleep(0.3)
        n0 = U.item_count(main)
        U.key(lv, 0x2E)                                       # VK_DELETE
        U.wait_for(lambda: U.item_count(main) == n0 - 1, 5, what="Del key")
        ok("Del key removes a finished download, file kept", os.path.exists(os.path.join(OUT, "done.bin")))

        # Ctrl+V command: two links on the clipboard are added at once
        saved_clip = U.get_clipboard()
        U.set_clipboard("see %s/r/big.bin?p=1\r\nand %s/r/big.bin?p=2 thanks" % (base, base))
        time.sleep(0.5)
        ok("multi-line clipboard text does not pop up the Add dialog", not U.dialogs(p.pid))
        n0 = U.item_count(main)
        U.command(main, "PASTE")
        U.wait_for(lambda: U.item_count(main) == n0 + 2, 5, what="pasted links")
        ok("Paste links adds every link on the clipboard", True)

        # Ctrl+N command: the Add dialog is prefilled from the clipboard; OK adds them paused
        U.command(main, "ADD")
        ad = U.find_dialog(p.pid, "Add download", 5)
        txt = U.dlg_text(ad, 1001)
        ok("Add dialog prefilled with the clipboard links", txt.count("http://") == 2, repr(txt[:80]))
        U.user32.SendMessageW(U.user32.GetDlgItem(ad, 1009), 0x00F5, 0, 0)    # BM_CLICK "Add paused"
        n0 = U.item_count(main)
        U.user32.PostMessageW(ad, U.WM_COMMAND, 1, 0)                         # IDOK
        U.wait_for(lambda: U.item_count(main) == n0 + 2, 5, what="added from dialog")
        def dialog_paused():
            return [r for r in U.read_list(DATA) if r.get("state") == "paused" and "p=" in r.get("url", "")]
        try:
            U.wait_for(lambda: len(dialog_paused()) >= 2, 6, 0.5, what="paused entries saved")
        except TimeoutError:
            pass
        paused = dialog_paused()
        ok("Add dialog adds paused downloads when asked", len(paused) >= 2, "%d paused" % len(paused))
        if saved_clip is not None:
            U.set_clipboard(saved_clip)

        # settings: max simultaneous = 1, then two queued downloads -> one runs
        U.command(main, "SETTINGS")
        sd = U.find_dialog(p.pid, "Settings", 5)
        edit = U.user32.GetDlgItem(sd, 1105)
        buf = ctypes.create_unicode_buffer("1")
        U.user32.SendMessageW(edit, 0x000C, 0, ctypes.addressof(buf))   # WM_SETTEXT
        U.user32.PostMessageW(sd, U.WM_COMMAND, 1, 0)                   # OK
        U.wait_for(lambda: not U.dialogs(p.pid), 5, what="settings closed")
        st = open(os.path.join(DATA, "settings.txt"), encoding="utf-8").read()
        ok("settings saved", "max_active=1" in st)
        U.send_job(main, U.write_job(os.path.join(JOBS, "d.txt"), url=base + "/r/big30.bin?kbps=150", dir=OUT,
                                     name="q1.bin"))
        U.send_job(main, U.write_job(os.path.join(JOBS, "e.txt"), url=base + "/r/big30.bin?kbps=150", dir=OUT,
                                     name="q2.bin"))
        time.sleep(1.5)
        sts = {r.get("out_name"): r.get("state") for r in U.read_list(DATA)}
        ok("max simultaneous = 1 is respected", sts.get("slow1.bin") == "running" and
           sts.get("q1.bin") == "queued" and sts.get("q2.bin") == "queued", str(sts))

        # exit while a dialog is open: the dialog closes and the app exits cleanly
        U.command(main, "SETTINGS")
        U.find_dialog(p.pid, "Settings", 5)
        t0 = time.time()
        U.command(main, "EXIT")
        p.wait(15)
        ok("exit with the settings dialog open", p.returncode == 0, "%.1fs" % (time.time() - t0))
        sts = {r.get("out_name"): r.get("state") for r in U.read_list(DATA)}
        ok("running download saved as paused, queued ones as queued (they load back paused)",
           sts.get("slow1.bin") == "paused" and sts.get("q1.bin") == "queued", str(sts))
        p = U.start_gui(EXE, DATA, env_extra={"ULOADER_TRACE": TRACE})
        main = U.find_main()
        time.sleep(1.5)
        sts = {r.get("out_name"): r.get("state") for r in U.read_list(DATA)}
        ok("nothing starts by itself after a restart", not any(
            n and os.path.exists(os.path.join(OUT, n + ".ulpart")) and v == "running" for n, v in sts.items()) and
           all(sts.get(k) == "paused" for k in ("slow1.bin", "q1.bin", "q2.bin")) and
           all(v in ("paused", "done") for v in sts.values()), str(sts))
        U.command(main, "EXIT")
        p.wait(15)
    finally:
        if p.poll() is None:
            p.kill()
        srv.kill()
    print("\n%d/%d checks passed" % (sum(1 for r in results if r[1]), len(results)))
    stray = [ln.strip() for ln in open(TRACE) if " activate " in ln] if os.path.exists(TRACE) else []
    print("activations not caused by the test:", stray or "none")


main()
