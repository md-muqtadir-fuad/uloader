"""End-to-end test of uloader-gui.exe against a local throttling server."""
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest as U  # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))
PROJ = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
EXE = os.path.join(HERE, "build", "uloader-gui.exe")
FIX = os.path.abspath(os.path.join(HERE, "..", "fixture"))
T = os.path.join(HERE, "t1")
DATA, OUT, JOBS, SHOTS = (os.path.join(T, d) for d in ("data", "out", "jobs", "shots"))
BIG30, BIG = os.path.join(FIX, "big30.bin"), os.path.join(FIX, "big.bin")
TRACE = os.path.join(T, "trace.txt")
TR = {"ULOADER_TRACE": TRACE}

results = []
pings = []


def ok(name, cond, detail=""):
    results.append((name, bool(cond), detail))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)
    return cond


def rec(name):
    return next((r for r in U.read_list(DATA) if r.get("name") == name or
                 os.path.basename(r.get("path", "")) == name or r.get("out_name") == name), None)


def wait_state(name, state, timeout):
    return U.wait_for(lambda: (rec(name) or {}).get("state") == state, timeout, 0.25,
                      what="%s -> %s" % (name, state))


def complete(path):
    return os.path.exists(path) and not os.path.exists(path + ".ulpart")


def probe(main, n=1):
    for _ in range(n):
        p = U.ping(main)
        pings.append(p if p is not None else 99999)


def stall_server(port, stop):
    """Accepts connections and never answers."""
    s = socket.socket()
    s.bind(("127.0.0.1", port))
    s.listen(16)
    s.settimeout(0.5)
    conns = []
    while not stop.is_set():
        try:
            c, _ = s.accept()
            conns.append(c)
        except socket.timeout:
            pass
    for c in conns:
        c.close()
    s.close()


def main():
    shutil.rmtree(T, ignore_errors=True)
    for d in (DATA, OUT, JOBS, SHOTS):
        os.makedirs(d)
    # settings: save into OUT by default (so forwarded links do not land in the real Downloads folder)
    with open(os.path.join(DATA, "settings.txt"), "w", encoding="utf-8") as f:
        f.write("dir=%s\n" % OUT)

    port = U.free_port()
    srvlog = os.path.join(T, "server.log")
    srv = subprocess.Popen([sys.executable, "-I", os.path.join(PROJ, "tests", "server.py"), str(port), FIX, srvlog])
    stall_port = U.free_port()
    stop = threading.Event()
    threading.Thread(target=stall_server, args=(stall_port, stop), daemon=True).start()
    base = "http://127.0.0.1:%d" % port
    time.sleep(0.8)
    procs = []
    clip_saved = U.get_clipboard()
    try:
        # ---------------- 1. start with --job --------------------------------------------
        j1 = U.write_job(os.path.join(JOBS, "job1.txt"), url=base + "/r/big30.bin", dir=OUT)
        t0 = time.time()
        p = U.start_gui(EXE, DATA, "--job", j1, env_extra=TR)
        procs.append(p)
        main = U.find_main()
        U.wait_for(lambda: complete(os.path.join(OUT, "big30.bin")), 60, what="job1 file")
        ok("job via --job: file arrived", True, "%.1fs" % (time.time() - t0))
        ok("job via --job: SHA-256 matches", U.sha256(os.path.join(OUT, "big30.bin")) == U.sha256(BIG30),
           U.sha256(BIG30)[:16])
        ok("job file deleted after load", not os.path.exists(j1))
        probe(main, 3)

        # ---------------- 2. second job via WM_COPYDATA, with a bridge cookie file ---------
        cookie_tmp = os.path.join(tempfile.gettempdir(), "ulc_guitest_%d.tmp" % os.getpid())
        with open(cookie_tmp, "w") as f:
            f.write("# Netscape HTTP Cookie File\n127.0.0.1\tFALSE\t/\tFALSE\t0\tsid\tabc123\n")
        j2 = U.write_job(os.path.join(JOBS, "job2.txt"), url=base + "/r/big.bin", dir=OUT, name="second.bin",
                         connections=4, cookies=cookie_tmp, referer="http://example.test/page")
        ok("WM_COPYDATA accepted", U.send_job(main, j2))
        U.wait_for(lambda: complete(os.path.join(OUT, "second.bin")), 60, what="job2 file")
        ok("WM_COPYDATA job: SHA-256 matches", U.sha256(os.path.join(OUT, "second.bin")) == U.sha256(BIG))
        U.wait_for(lambda: not os.path.exists(cookie_tmp), 10, what="cookie file removal")
        ok("bridge cookie file deleted after completion", not os.path.exists(cookie_tmp))
        log = open(srvlog, encoding="utf-8").read()
        ok("cookie + referer sent by the engine", "sid=abc123" in log and "example.test" in log)
        ok("bad WM_COPYDATA rejected", not U.send_job(main, os.path.join(JOBS, "does-not-exist.txt")))

        # ---------------- 3. pause / resume through WM_COMMAND -----------------------------
        j3 = U.write_job(os.path.join(JOBS, "job3.txt"), url=base + "/r/big30.bin?kbps=500", dir=OUT,
                         name="paused.bin", connections=4)
        U.send_job(main, j3)
        time.sleep(3.0)
        probe(main, 3)
        ok("new download is selected", U.selected_count(main) == 1)
        U.command(main, "PAUSE")
        t0 = time.time()
        wait_state("paused.bin", "paused", 20)
        pause_s = time.time() - t0
        part = os.path.join(OUT, "paused.bin.ulpart")
        state = os.path.join(OUT, "paused.bin.ulstate")
        done1 = int(rec("paused.bin").get("done", "0"))
        ok("pause: thread stopped with resumable state", os.path.exists(part) and os.path.exists(state)
           and not os.path.exists(os.path.join(OUT, "paused.bin")), "%.1fs, %d bytes kept" % (pause_s, done1))
        st1 = open(state).read()
        time.sleep(2.0)
        ok("pause: nothing written while paused", open(state).read() == st1)
        U.command(main, "RESUME")
        U.wait_for(lambda: complete(os.path.join(OUT, "paused.bin")), 90, what="resumed file")
        wait_state("paused.bin", "done", 10)
        ok("resume: SHA-256 matches", U.sha256(os.path.join(OUT, "paused.bin")) == U.sha256(BIG30))
        srv_lines = [ln for ln in open(srvlog, encoding="utf-8") if "kbps=500" in ln]
        ok("resume used byte ranges after the pause", len(srv_lines) > 4, "%d range requests" % len(srv_lines))

        # ---------------- 4. a stalled server: UI stays responsive, delete while connecting --
        j4 = U.write_job(os.path.join(JOBS, "job4.txt"), url="http://127.0.0.1:%d/stall.zip" % stall_port,
                         dir=OUT, name="stalled.zip")
        U.send_job(main, j4)
        n_before = U.item_count(main)
        worst = 0
        for _ in range(10):
            time.sleep(0.3)
            p_ms = U.ping(main)
            worst = max(worst, p_ms if p_ms is not None else 99999)
        ok("UI responsive while a connection stalls", worst < 250, "worst ping %.1f ms" % worst)
        U.command(main, "DELETE")       # selected = the stalled one
        U.wait_for(lambda: U.item_count(main) == n_before - 1, 5, what="row removed")
        ok("delete while connecting removes the row at once", True)
        probe(main, 5)
        j5 = U.write_job(os.path.join(JOBS, "job5.txt"), url="http://127.0.0.1:%d/stall2.zip" % stall_port,
                         dir=OUT, name="stalled2.zip")
        U.send_job(main, j5)
        time.sleep(1.5)
        U.command(main, "PAUSE")
        t0 = time.time()
        wait_state("stalled2.zip", "paused", 45)
        ok("pause of a download stuck on a stalled connection", True,
           "%.1fs until saved as paused (includes the 0.6 s save delay)" % (time.time() - t0))
        n_before = U.item_count(main)
        U.command(main, "DELETE")
        U.wait_for(lambda: U.item_count(main) == n_before - 1, 5, what="stalled2 removed")

        # ---------------- 5. screenshot scenario -------------------------------------------
        jobs = [
            dict(url=base + "/status/404", name="missing-file.zip"),
            dict(url=base + "/r/big30.bin?kbps=160", name="Holiday video 2026 (4K).mp4", connections=8),
            dict(url=base + "/r/big30.bin?kbps=300", name="ubuntu-24.04-desktop-amd64.iso", connections=4),
            dict(url=base + "/r/big.bin?kbps=45", name="Annual report.pdf", connections=2),
            dict(url=base + "/r/big30.bin?kbps=100", name="setup-x64.exe", connections=8),
            dict(url=base + "/r/big30.bin?kbps=100", name="photos-backup.zip", connections=8),
        ]
        for i, kv in enumerate(jobs):
            kv["dir"] = OUT
            U.send_job(main, U.write_job(os.path.join(JOBS, "s%d.txt" % i), **kv))
            time.sleep(0.15)
        lv = U.listview(main)
        U.key(lv, U.VK_UP)               # select setup-x64.exe (queued) and pause it
        time.sleep(0.4)
        U.command(main, "PAUSE")
        time.sleep(0.4)
        U.key(lv, U.VK_HOME)
        U.key(lv, U.VK_DOWN, 4)          # the 8-connection video
        time.sleep(6.0)
        probe(main, 3)
        U.capture(main, os.path.join(SHOTS, "main_downloading.png"))
        recs = U.read_list(DATA)
        states = {r.get("out_name") or r.get("name"): r.get("state") for r in recs}
        ok("queue: at most 3 running, rest queued/paused",
           list(states.values()).count("running") == 3 and states.get("photos-backup.zip") == "queued" and
           states.get("setup-x64.exe") == "paused", str(states))
        ok("404 shows as error", states.get("missing-file.zip") == "error")

        # ---------------- 6. clipboard watch -> Add dialog ---------------------------------
        link = base + "/files/new-release-2.1.zip"
        U.set_clipboard(link)
        dlg = U.find_dialog(p.pid, "Add download", 5)
        ok("clipboard watch opens the Add dialog", dlg and link in U.dlg_text(dlg, 1001))
        time.sleep(0.5)
        U.capture(dlg, os.path.join(SHOTS, "add_dialog.png"))
        U.user32.PostMessageW(dlg, U.WM_COMMAND, U.IDCANCEL, 0)
        U.wait_for(lambda: not U.dialogs(p.pid), 5, what="dialog closed")
        U.set_clipboard("just some text")
        time.sleep(0.5)
        U.set_clipboard(link)
        time.sleep(1.5)
        ok("same link is not offered twice", not U.dialogs(p.pid))
        U.set_clipboard(base + "/page/readme.html")
        time.sleep(1.0)
        ok("non-download link is ignored", not U.dialogs(p.pid))

        # ---------------- 7. settings dialog -----------------------------------------------
        U.command(main, "SETTINGS")
        sd = U.find_dialog(p.pid, "Settings", 5)
        time.sleep(0.5)
        U.capture(sd, os.path.join(SHOTS, "settings_dialog.png"))
        U.user32.PostMessageW(sd, U.WM_COMMAND, 1110, 0)      # Connect to browsers (no uloader.exe here)
        time.sleep(1.5)
        out = U.dlg_text(sd, 1115)
        ok("connect-to-browsers runs off the UI thread and reports", "uloader.exe" in out, out.strip()[:90])
        U.capture(sd, os.path.join(SHOTS, "settings_dialog_output.png"))
        U.user32.PostMessageW(sd, U.WM_COMMAND, U.IDCANCEL, 0)
        U.wait_for(lambda: not U.dialogs(p.pid), 5, what="settings closed")
        probe(main, 3)

        # ---------------- 8. single instance: a second process forwards its link -----------
        n0 = U.item_count(main)
        t0 = time.time()
        p2 = U.start_gui(EXE, DATA, base + "/r/big.bin")
        p2.wait(15)
        ok("second instance forwards and exits", p2.returncode == 0 and U.item_count(main) == n0 + 1,
           "%.2fs" % (time.time() - t0))
        U.wait_for(lambda: complete(os.path.join(OUT, "big.bin")), 180, what="forwarded link")
        ok("forwarded link: SHA-256 matches", U.sha256(os.path.join(OUT, "big.bin")) == U.sha256(BIG))

        # ---------------- 9. close to tray, exit while downloading, restart ------------------
        U.command(main, "PAUSEALL")
        time.sleep(0.5)
        jr = U.write_job(os.path.join(JOBS, "jr.txt"), url=base + "/r/big30.bin?kbps=200", dir=OUT,
                         name="resume-after-restart.bin", connections=4)
        U.send_job(main, jr)
        time.sleep(4.0)
        U.user32.PostMessageW(main, U.WM_CLOSE, 0, 0)
        time.sleep(1.0)
        ok("WM_CLOSE hides to the tray (setting on)", not U.user32.IsWindowVisible(main) and p.poll() is None)
        n_saved = U.item_count(main)
        t0 = time.time()
        U.command(main, "EXIT")
        p.wait(15)
        ok("exit stops downloads and quits", p.returncode == 0, "%.1fs" % (time.time() - t0))
        recs = U.read_list(DATA)
        r = rec("resume-after-restart.bin")
        ok("list saved on exit", len(recs) == n_saved and r and r["state"] == "paused",
           "%d entries, running one saved as %s" % (len(recs), r and r["state"]))

        p = U.start_gui(EXE, DATA, env_extra=TR)
        procs.append(p)
        main = U.find_main()
        time.sleep(1.0)
        ok("list restored after restart", U.item_count(main) == n_saved, "%d rows" % U.item_count(main))
        lv = U.listview(main)
        U.key(lv, U.VK_END)
        time.sleep(1.0)
        U.capture(main, os.path.join(SHOTS, "main_restored.png"))
        U.command(main, "RESUME")
        U.wait_for(lambda: complete(os.path.join(OUT, "resume-after-restart.bin")), 120, what="resume after restart")
        ok("resume after restart: SHA-256 matches",
           U.sha256(os.path.join(OUT, "resume-after-restart.bin")) == U.sha256(BIG30))
        probe(main, 3)
        time.sleep(1.0)
        U.capture(main, os.path.join(SHOTS, "main_after.png"))
        U.command(main, "EXIT")
        p.wait(15)
        ok("second exit clean", p.returncode == 0)
    finally:
        for pr in procs:
            if pr.poll() is None:
                pr.kill()
        srv.kill()
        stop.set()
        if clip_saved is not None:
            try:
                U.set_clipboard(clip_saved)
            except Exception:
                pass
    if os.path.exists(TRACE):
        stray = [ln.strip() for ln in open(TRACE) if " activate " in ln]
        print("activations not caused by the test (mouse/keyboard on the window):", stray or "none")
    good = [r for r in results if r[1]]
    print("\n%d/%d checks passed; UI ping max %.1f ms over %d probes" %
          (len(good), len(results), max(pings) if pings else -1, len(pings)))
    return 0 if len(good) == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
