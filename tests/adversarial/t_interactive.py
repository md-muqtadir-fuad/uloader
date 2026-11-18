"""Interactive-mode tests: Ctrl+C while a connection is stalled, then keep using the session.
python -I t_interactive.py [name-substring ...]
Uses adv/harness/sendctrl.exe to deliver Ctrl+Break (or Ctrl+C with CTRL_KIND=c) to uloader's own (hidden) console."""
import os
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

SENDCTRL = os.path.join(ADV, "harness", "sendctrl.exe")
CTRL_KIND = os.environ.get("CTRL_KIND", "break")   # "break": the test runner may inherit "ignore Ctrl+C"
MiB = 1 << 20


class Session:
    def __init__(self, args, cwd):
        si = subprocess.STARTUPINFO()
        si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
        si.wShowWindow = 0
        self.p = subprocess.Popen([EXE] + args, cwd=cwd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, creationflags=subprocess.CREATE_NEW_CONSOLE,
                                  startupinfo=si)
        self.buf = b""
        self.t = threading.Thread(target=self._rd, daemon=True)
        self.t.start()

    def _rd(self):
        while True:
            b = self.p.stdout.read1(4096) if hasattr(self.p.stdout, "read1") else self.p.stdout.read(1)
            if not b:
                break
            self.buf += b

    def send(self, line):
        self.p.stdin.write((line + "\n").encode())
        self.p.stdin.flush()

    def ctrl_c(self):
        subprocess.call([SENDCTRL, str(self.p.pid), CTRL_KIND])

    def text(self):
        return self.buf.decode("utf-8", "replace").replace("\r", "\n")

    def wait_for(self, s, timeout):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if s in self.text() or self.p.poll() is not None:
                return s in self.text()
            time.sleep(0.2)
        return False

    def close(self):
        try:
            self.send("q")
        except OSError:
            pass
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            self.p.kill()
            self.p.wait()


def t_pause_with_stalled_connection_then_resume():
    d = fresh_dir("inter_direct")
    srv, out = os.path.join(d, "srv"), os.path.join(d, "out")
    os.makedirs(srv)
    os.makedirs(out)
    make_file_fast(os.path.join(srv, "a.bin"), 16 * MiB, 3)
    S = Server(srv)
    try:
        url = S.url("/f/a.bin?stall=ranged&delay=0.05")
        s = Session(["-c", "4", "-t", "30", "-d", out], out)
        s.wait_for("Paste a link", 10)
        s.send(url)
        time.sleep(3)
        s.ctrl_c()
        paused = s.wait_for("paused", 15)
        time.sleep(1)
        # the user does what the message says: run it again (same session)
        open(os.path.join(srv, "_opts"), "w").write("")       # server healthy now
        s.send(S.url("/f/a.bin?stall=ranged&delay=0.05"))
        time.sleep(8)
        txt = s.text()
        after = txt.split("paused", 1)[1] if "paused" in txt else txt
        alive = s.p.poll() is None
        s.close()
        bad = "cannot create" in after or not alive
        report("interactive: Ctrl+C with stalled connection, then resume same URL", not bad,
               "paused=%s still_alive=%s exit=%s; after pause: %s" % (
                   paused, alive, s.p.returncode, " | ".join(l.strip() for l in after.splitlines() if "Error" in l or "cannot" in l)[:300]))
    finally:
        S.stop()


def t_hls_pause_with_stalled_segment():
    """Ctrl+C while HLS segment reads are stalled: hls_fetch_media frees its context
    after 3 s although the workers are still running; they wake up later."""
    d = fresh_dir("inter_hls")
    srv, out = os.path.join(d, "srv"), os.path.join(d, "out")
    os.makedirs(os.path.join(srv, "h"))
    os.makedirs(out)
    n = 12
    for tag in ("x", "y"):
        for i in range(n):
            make_file_fast(os.path.join(srv, "h", "s%s%02d.ts" % (tag, i)), 300000, i)
        pl = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n" + "".join("#EXTINF:2,\ns%s%02d.ts\n" % (tag, i) for i in range(n)) + "#EXT-X-ENDLIST\n"
        open(os.path.join(srv, "h", "%s.m3u8" % tag), "w", newline="\n").write(pl)
    S = Server(srv)
    try:
        s = Session(["-c", "4", "-t", "8", "-d", out], out)
        s.wait_for("Paste a link", 10)
        open(os.path.join(srv, "_opts"), "w").write("stall=ts")  # every segment request hangs after headers
        s.send(S.url("/f/h/x.m3u8"))
        time.sleep(2)
        s.ctrl_c()
        s.wait_for("paused", 15)
        t_pause = time.strftime("%H:%M:%S")
        nreq_at_pause = len(S.requests())
        open(os.path.join(srv, "_opts"), "w").write("")
        s.send(S.url("/f/h/y.m3u8"))
        time.sleep(20)
        alive = s.p.poll() is None
        code = s.p.returncode
        late = [l for l in S.requests()[nreq_at_pause:] if "/sx" in l]
        cache = os.path.join(out, "h.ulhls")
        segs_after = sorted(os.listdir(cache)) if os.path.isdir(cache) else []
        txt = s.text()
        s.close()
        report("interactive: HLS pause with stalled segment, keep using session", alive and not late,
               "alive=%s exit=%s; requests for the PAUSED stream after 'paused': %d (e.g. %s); its cache now: %s" % (
                   alive, code if code is None else hex(code & 0xFFFFFFFF), len(late), late[:2], segs_after[:6]))
    finally:
        S.stop()


TESTS = [t_pause_with_stalled_connection_then_resume, t_hls_pause_with_stalled_segment]

if __name__ == "__main__":
    sel = sys.argv[1:]
    for t in TESTS:
        if sel and not any(x in t.__name__ for x in sel):
            continue
        try:
            t()
        except Exception as ex:
            import traceback
            traceback.print_exc()
            report(t.__name__, False, "harness exception: %r" % ex)
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
