"""FTP test matrix for U-loader (Python 3.8, stdlib only).

    python -I run_tests.py                 all local tests
    python -I run_tests.py basic drop_fin  only these
    python -I run_tests.py real            public-server sanity check (needs internet)
    python -I run_tests.py --list

Uses uloader.exe and ftpserver.py next to this script (ULOADER_EXE overrides
the exe), test files in ./root, scratch output in ./work.
"""
import ctypes
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.environ.get("ULOADER_EXE") or os.path.join(HERE, "uloader.exe")
SERVER = os.path.join(HERE, "ftpserver.py")
ROOT = os.path.join(HERE, "root")
WORK = os.path.join(HERE, "work")
SENDCTRLC = os.path.join(os.path.dirname(HERE), "tools", "sendctrlc.exe")

UNI_DIR = "sub dir"
UNI_NAME = "ünï cødé 文件.bin"       # "ünï cødé 文件.bin"
UNI_PCT = "pub/sub%20dir/%C3%BCn%C3%AF%20c%C3%B8d%C3%A9%20%E6%96%87%E4%BB%B6.bin"
FILES = {
    "big.bin": 30 * 1024 * 1024 + 17,
    "pub/%s/%s" % (UNI_DIR, UNI_NAME): 3 * 1024 * 1024 + 123,
    "empty.bin": 0,
    "home/alice/secret.bin": 2 * 1024 * 1024 + 5,
}
HASH = {}
RESULTS = []


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def make_fixtures():
    for rel, size in FILES.items():
        p = os.path.join(ROOT, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        if not os.path.isfile(p) or os.path.getsize(p) != size:
            with open(p, "wb") as f:
                left = size
                while left:
                    n = min(left, 1 << 20)
                    f.write(os.urandom(n))
                    left -= n
        HASH[rel] = sha(p)


class Srv:
    def __init__(self, *args, name="srv"):
        self.stats_path = os.path.join(WORK, name + ".stats.json")
        self.log_path = os.path.join(WORK, name + ".server.log")
        for p in (self.stats_path, self.log_path):
            if os.path.exists(p):
                os.remove(p)
        cmd = [sys.executable, "-I", SERVER, "--root", ROOT, "--stats", self.stats_path,
               "--log", self.log_path] + [str(a) for a in args]
        self.p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        line = self.p.stdout.readline().decode()
        if not line.startswith("PORT "):
            raise RuntimeError("server did not start: %r" % line)
        self.port = int(line.split()[1])

    def stats(self):
        for _ in range(20):
            try:
                with open(self.stats_path) as f:
                    return json.load(f)
            except (OSError, ValueError):
                time.sleep(0.05)
        return {}

    def stop(self):
        time.sleep(0.3)             # let the last sessions write their stats
        st = self.stats()
        self.p.kill()
        self.p.wait()
        return st

    def __enter__(self):
        return self

    def __exit__(self, *a):
        if self.p.poll() is None:
            self.p.kill()
            self.p.wait()


def fresh(name):
    d = os.path.join(WORK, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    return d


def ul(args, cwd, timeout=300):
    t0 = time.time()
    try:
        p = subprocess.run([EXE] + [str(a) for a in args], cwd=cwd, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=timeout)
        rc, out = p.returncode, p.stdout
    except subprocess.TimeoutExpired as e:
        rc, out = "HANG", e.output or b""
    return rc, clean(out.decode("utf-8", "replace")), time.time() - t0


def clean(out):
    lines = []
    for l in out.replace("\r", "\n").split("\n"):
        l = l.strip()
        if l and "█" not in l and "░" not in l and not l.startswith("100.0%"):
            lines.append(l)
    return "\n".join(lines)


def spawn_console(args, cwd, logpath):
    """uloader in its own (hidden) console, so a real Ctrl+C can be sent to it."""
    # "Ignore Ctrl+C" is inherited by child processes; when this runner was
    # started from a tool that set it, uloader would never see the Ctrl+C.
    ctypes.windll.kernel32.SetConsoleCtrlHandler(None, False)
    si = subprocess.STARTUPINFO()
    si.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    si.wShowWindow = 0
    lf = open(logpath, "wb")
    p = subprocess.Popen([EXE] + [str(a) for a in args], cwd=cwd, stdout=lf, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, creationflags=subprocess.CREATE_NEW_CONSOLE, startupinfo=si)
    return p, lf


def ctrl_c(pid):
    return subprocess.run([SENDCTRLC, str(pid)], timeout=15).returncode


def leftovers(d):
    return [f for f in os.listdir(d) if f.endswith(".ulpart") or f.endswith(".ulstate")]


def record(name, ok, detail):
    RESULTS.append((name, ok, detail))
    print("[%s] %-20s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)


def last_lines(out, n=3):
    return " | ".join(out.splitlines()[-n:])


def summary(st):
    keys = [("peak", "peak"), ("logged_in_peak", "maxlogin"), ("busy", "busy"), ("drops", "drops"), ("stalls", "stalls"),
            ("retr", "retr"), ("pasv", "pasv"), ("epsv", "epsv"), ("bad_login", "badlogin")]
    return " ".join("%s=%s" % (label, st.get(k)) for k, label in keys if st.get(k))


# ----------------------------------------------------------------------
def expect_ok(name, srv_args, path, rel, ul_args=("-c", "8"), url=None, fname=None, want=None, timeout=300):
    d = fresh(name)
    with Srv(*srv_args, name=name) as s:
        u = url(s.port) if url else "ftp://127.0.0.1:%d/%s" % (s.port, path)
        rc, out, dt = ul(list(ul_args) + ["-d", d, u], d, timeout)
        st = s.stop()
    f = os.path.join(d, fname or rel.split("/")[-1])
    got = sha(f) if os.path.isfile(f) else None
    ok = rc == 0 and got == HASH[rel] and not leftovers(d)
    if want and want not in out:
        ok = False
    detail = "rc=%s %.1fs sha=%s %s" % (rc, dt, (got or "missing")[:12], summary(st))
    if not ok:
        detail += "\n      " + last_lines(out, 6)
    record(name, ok, detail)
    return ok, dt, out


def expect_fail(name, srv_args, path, must, ul_args=("-c", "8"), url=None, max_time=60, fname=None):
    d = fresh(name)
    with Srv(*srv_args, name=name) as s:
        u = url(s.port) if url else "ftp://127.0.0.1:%d/%s" % (s.port, path)
        rc, out, dt = ul(list(ul_args) + ["-d", d, u], d, max_time + 60)
        st = s.stop()
    fname = fname or path.rstrip("/").split("/")[-1]
    complete = [f for f in os.listdir(d) if f == fname]
    ok = rc == 1 and must.lower() in out.lower() and not complete and dt <= max_time
    msg = [l for l in out.splitlines() if "fail" in l.lower() or "error" in l.lower() or "answered" in l.lower()
           or "ftp" in l.lower()]
    detail = "rc=%s %.1fs msg=%r %s" % (rc, dt, (msg[-1] if msg else last_lines(out, 2))[:150], summary(st))
    record(name, ok, detail)
    return ok


# ----------------------------------------------------------------------
BIG = "big.bin"
UNI = "pub/%s/%s" % (UNI_DIR, UNI_NAME)


def t_basic():
    expect_ok("basic_c8", [], BIG, BIG)


def t_basic_c1():
    expect_ok("basic_c1", [], BIG, BIG, ul_args=("-c", "1"))


def t_c32():
    expect_ok("many_c32", [], BIG, BIG, ul_args=("-c", "32"))


def t_no_rest():
    expect_ok("no_rest", ["--no-rest"], BIG, BIG, want="not resumable")


def t_no_size():
    expect_ok("no_size", ["--no-size"], BIG, BIG, want="size unknown")


def t_no_epsv():
    expect_ok("no_epsv", ["--no-epsv"], BIG, BIG)


def t_pasv_ip():
    expect_ok("pasv_wrong_ip", ["--no-epsv", "--pasv-ip", "10.255.255.1"], BIG, BIG)


def t_bad_epsv():
    expect_ok("epsv_dead_port", ["--bad-epsv", "1"], BIG, BIG)


def t_max421():
    expect_ok("max_conns_421", ["--max-conns", "3"], BIG, BIG)


def t_max530():
    expect_ok("max_conns_530", ["--max-conns", "3", "--limit-style", "530"], BIG, BIG)


def t_max421_slow():
    expect_ok("max_conns_421_thr", ["--max-conns", "3", "--throttle", "2000000"], BIG, BIG)


def t_drop_fin():
    expect_ok("drop_fin", ["--drop-after", "3000000", "--drop-times", "6"], BIG, BIG)


def t_drop_rst():
    expect_ok("drop_rst", ["--drop-after", "3000000", "--drop-times", "6", "--drop-mode", "rst"], BIG, BIG)


def t_drop_lie():
    expect_ok("drop_lie_226", ["--drop-after", "3000000", "--drop-times", "4", "--drop-lie"], BIG, BIG)


def t_drop_no_size():
    expect_ok("drop_no_size", ["--no-size", "--drop-after", "5000000", "--drop-times", "2"], BIG, BIG)


def t_drop_no_rest():
    expect_ok("drop_no_rest", ["--no-rest", "--drop-after", "5000000", "--drop-times", "2"], BIG, BIG)


def t_drop_always():
    expect_fail("drop_always_nosize", ["--no-size", "--drop-after", "5000000"], BIG, "download failed",
                ul_args=("-c", "8", "-R", "2"), max_time=60)


def t_stall():
    expect_ok("stall_timeout", ["--stall-after", "2000000", "--stall-times", "2"], BIG, BIG,
              ul_args=("-c", "8", "-t", "5"))


def t_size_lie():
    expect_fail("size_too_big", ["--size-delta", "1000"], BIG, "download failed",
                ul_args=("-c", "8", "-R", "2"), max_time=90)


def t_missing():
    expect_fail("missing_file", [], "nope.bin", "404", max_time=5)


def t_missing_dir():
    expect_fail("missing_dir", [], "no/such/dir/nope.bin", "404", max_time=5)


def t_bad_login():
    expect_fail("bad_login", ["--user", "bob", "--password", "secret"], BIG, "401", max_time=10,
                url=lambda p: "ftp://bob:wrong@127.0.0.1:%d/big.bin" % p)


def t_tls():
    expect_fail("tls_required", ["--tls-required"], BIG, "FTPS", max_time=20)


def t_folder():
    expect_fail("folder_url", [], "pub/", "folder", max_time=20, fname="pub")


def t_multiline():
    expect_ok("multiline_chunked", ["--multiline", "--chunked"], BIG, BIG)


def t_slow():
    expect_ok("slow_replies", ["--slow-reply", "0.05"], BIG, BIG)


def t_userpass():
    # user "us@r", password "p:ss w%rd/ü"
    expect_ok("userpass_encoded", ["--user", "us@r", "--password", "p:ss w%rd/ü"], BIG, BIG,
              url=lambda p: "ftp://us%%40r:p%%3Ass%%20w%%25rd%%2F%%C3%%BC@127.0.0.1:%d/big.bin" % p)


def t_uni_pct():
    expect_ok("unicode_pct", [], UNI_PCT, UNI)


def t_uni_raw():
    expect_ok("unicode_raw", [], "pub/%s/%s" % (UNI_DIR, UNI_NAME), UNI)


def t_home():
    expect_ok("home_relative", ["--home", "home/alice"], "secret.bin", "home/alice/secret.bin")


def t_no_abs():
    expect_ok("no_abs_cwd", ["--no-abs"], UNI_PCT, UNI)


def t_no_abs_no_size():
    expect_ok("no_abs_no_size", ["--no-abs", "--no-size"], UNI_PCT, UNI)


def t_ipv6():
    expect_ok("ipv6", ["--ipv6"], None, BIG, url=lambda p: "ftp://[::1]:%d/big.bin" % p)


def t_localhost():
    expect_ok("localhost_name", [], None, BIG, url=lambda p: "ftp://localhost:%d/big.bin" % p)


def t_empty():
    expect_ok("empty_file", [], "empty.bin", "empty.bin")


def t_type():
    expect_ok("type_i_suffix", [], "big.bin;type=i", BIG, ul_args=("-c", "8", "-o", "big.bin"))


def t_pause():
    name = "pause_ctrl_c"
    d = fresh(name)
    with Srv("--throttle", "600000", name=name + "1") as s:
        u = "ftp://127.0.0.1:%d/big.bin" % s.port
        p, lf = spawn_console(["-c", "8", "-d", d, u], d, os.path.join(WORK, name + ".1.log"))
        time.sleep(3.5)
        t0 = time.time()
        sc = ctrl_c(p.pid)
        try:
            rc1 = p.wait(timeout=30)
        except subprocess.TimeoutExpired:
            p.kill()
            rc1 = "HANG"
        stop_s = time.time() - t0
        lf.close()
        s.stop()
    part = os.path.join(d, "big.bin.ulpart")
    state = os.path.join(d, "big.bin.ulstate")
    have_state = os.path.isfile(state) and os.path.isfile(part)
    with Srv(name=name + "2") as s:
        rc2, out, dt = ul(["-c", "8", "-d", d, "ftp://127.0.0.1:%d/big.bin" % s.port], d)
        st = s.stop()
    f = os.path.join(d, "big.bin")
    got = sha(f) if os.path.isfile(f) else None
    resumed = [l for l in out.splitlines() if "resuming" in l]
    ok = sc == 0 and rc1 == 2 and stop_s < 3 and have_state and rc2 == 0 and got == HASH[BIG] and resumed
    record(name, ok, "sendctrlc=%s paused rc=%s in %.2fs state=%s | resume rc=%s %.1fs %s sha=%s" %
           (sc, rc1, stop_s, have_state, rc2, dt, resumed[0] if resumed else "NOT RESUMED", (got or "-")[:12]))


def t_kill():
    name = "hard_kill"
    d = fresh(name)
    with Srv("--throttle", "600000", name=name + "1") as s:
        u = "ftp://127.0.0.1:%d/big.bin" % s.port
        p, lf = spawn_console(["-c", "8", "-d", d, u], d, os.path.join(WORK, name + ".1.log"))
        time.sleep(4.0)
        p.kill()
        p.wait()
        lf.close()
        s.stop()
    with Srv(name=name + "2") as s:
        rc2, out, dt = ul(["-c", "8", "-d", d, "ftp://127.0.0.1:%d/big.bin" % s.port], d)
        st = s.stop()
    f = os.path.join(d, "big.bin")
    got = sha(f) if os.path.isfile(f) else None
    resumed = [l for l in out.splitlines() if "resuming" in l]
    ok = rc2 == 0 and got == HASH[BIG] and resumed and not leftovers(d)
    record(name, ok, "killed after 4s | rerun rc=%s %.1fs %s sha=%s" %
           (rc2, dt, resumed[0] if resumed else "NOT RESUMED", (got or "-")[:12]))


def t_abort_greeting():
    """Ctrl+C while the server never greets: must stop within ~1 s, not after the 30 s timeout."""
    name = "pause_no_greeting"
    d = fresh(name)
    with Srv("--no-greeting", name=name) as s:
        u = "ftp://127.0.0.1:%d/big.bin" % s.port
        p, lf = spawn_console(["-c", "8", "-t", "30", "-d", d, u], d, os.path.join(WORK, name + ".log"))
        time.sleep(2.0)
        t0 = time.time()
        sc = ctrl_c(p.pid)
        try:
            rc = p.wait(timeout=40)
        except subprocess.TimeoutExpired:
            p.kill()
            rc = "HANG"
        dt = time.time() - t0
        lf.close()
    ok = sc == 0 and rc == 2 and dt < 1.5
    record(name, ok, "sendctrlc=%s rc=%s stopped %.2fs after Ctrl+C" % (sc, rc, dt))


def t_speed():
    res = {}
    for c in (1, 4, 8):
        ok, dt, out = expect_ok("speed_c%d" % c, ["--throttle", "1500000"], BIG, BIG, ul_args=("-c", str(c)))
        res[c] = dt
    mb = FILES[BIG] / 1e6
    record("speed_summary", res[8] < res[1] / 3,
           "throttle 1.5 MB/s per connection, 31.5 MB: " +
           ", ".join("-c %d %.1fs (%.1f MB/s)" % (c, t, mb / t) for c, t in res.items()))


def t_real():
    d = fresh("real")
    tgt = "gnu/hello/hello-2.12.tar.gz"
    rc1, out1, dt1 = ul(["-c", "4", "-t", "20", "-d", d, "-o", "ftp.tar.gz", "ftp://ftp.gnu.org/" + tgt], d, 180)
    rc2, out2, dt2 = ul(["-c", "4", "-t", "20", "-d", d, "-o", "https.tar.gz", "https://ftp.gnu.org/" + tgt], d, 180)
    f1, f2 = os.path.join(d, "ftp.tar.gz"), os.path.join(d, "https.tar.gz")
    h1 = sha(f1) if os.path.isfile(f1) else None
    h2 = sha(f2) if os.path.isfile(f2) else None
    ok = rc1 == 0 and h1 and h1 == h2
    record("real_ftp_gnu", ok, "ftp rc=%s %.1fs sha=%s | https rc=%s sha=%s\n      %s" %
           (rc1, dt1, h1, rc2, h2, last_lines(out1, 4)))


TESTS = [
    ("basic", t_basic), ("basic_c1", t_basic_c1), ("c32", t_c32),
    ("no_rest", t_no_rest), ("no_size", t_no_size), ("no_epsv", t_no_epsv), ("pasv_ip", t_pasv_ip),
    ("bad_epsv", t_bad_epsv), ("max421", t_max421), ("max530", t_max530), ("max421_slow", t_max421_slow),
    ("drop_fin", t_drop_fin), ("drop_rst", t_drop_rst), ("drop_lie", t_drop_lie),
    ("drop_no_size", t_drop_no_size), ("drop_no_rest", t_drop_no_rest), ("drop_always", t_drop_always),
    ("stall", t_stall), ("size_lie", t_size_lie),
    ("missing", t_missing), ("missing_dir", t_missing_dir), ("bad_login", t_bad_login), ("tls", t_tls),
    ("folder", t_folder), ("multiline", t_multiline), ("slow", t_slow), ("userpass", t_userpass),
    ("uni_pct", t_uni_pct), ("uni_raw", t_uni_raw), ("home", t_home), ("no_abs", t_no_abs),
    ("no_abs_no_size", t_no_abs_no_size), ("ipv6", t_ipv6), ("localhost", t_localhost),
    ("empty", t_empty), ("type", t_type), ("pause", t_pause), ("kill", t_kill),
    ("abort_greeting", t_abort_greeting), ("speed", t_speed),
]
EXTRA = [("real", t_real)]


def main():
    args = sys.argv[1:]
    alltests = dict(TESTS + EXTRA)
    if "--list" in args:
        print(" ".join(n for n, _ in TESTS + EXTRA))
        return 0
    os.makedirs(WORK, exist_ok=True)
    make_fixtures()
    names = args or [n for n, _ in TESTS]
    t0 = time.time()
    for n in names:
        alltests[n]()
    bad = [r for r in RESULTS if not r[1]]
    print("\n%d checks, %d failed, %.0fs" % (len(RESULTS), len(bad), time.time() - t0))
    for r in bad:
        print("  FAILED: %s" % r[0])
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
