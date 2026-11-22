"""Shared helpers for the U-loader adversarial tests (python -I)."""
import hashlib
import os
import random
import shutil
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ADV = os.path.dirname(HERE)
SCRATCH = os.path.dirname(ADV)
SNAP = os.path.join(SCRATCH, "review-v1")
EXE = os.environ.get("ULOADER_EXE", os.path.join(SNAP, "uloader.exe"))
RUNS = os.path.join(ADV, "runs")
# every U-loader these tests start (also the native host) keeps its data here,
# never in the real %APPDATA%\U-loader
os.environ.setdefault("ULOADER_DATA", os.path.join(RUNS, "_data"))


def ulvid(urls, ext="mp4", vext=None, aext=None, headers=None, aheaders=None, title="video", vid="id"):
    """One line of U-loader's yt-dlp --print template (ULVID2): tab-separated
    JSON values; a missing value is NA, as yt-dlp prints it."""
    import json

    def j(v):
        return "NA" if v is None else json.dumps(v)
    return "\t".join(["ULVID2", j(ext), j(vext), j(aext), j(headers if headers is not None else {}),
                      j(aheaders), j(title), j(vid), j("\n".join(urls))]) + "\n"


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


class Server:
    def __init__(self, root, script="evilserver.py", port=None):
        self.root = root
        self.port = port or free_port()
        self.log = os.path.join(root, "_server.log")
        open(self.log, "w").close()
        self.p = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, script), str(self.port), root, self.log],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        for _ in range(100):
            try:
                socket.create_connection(("127.0.0.1", self.port), 0.2).close()
                break
            except OSError:
                time.sleep(0.05)

    def url(self, path):
        return "http://127.0.0.1:%d%s" % (self.port, path)

    def requests(self):
        return open(self.log, encoding="utf-8").read().splitlines()

    def stop(self):
        self.p.kill()
        self.p.wait()


def fresh_dir(name):
    d = os.path.join(RUNS, name)
    shutil.rmtree("\\\\?\\" + d, ignore_errors=True)      # \\?\ so long paths go too
    os.makedirs(d)
    return d


def make_file(path, size, seed=1):
    r = random.Random(seed)
    with open(path, "wb") as f:
        left = size
        while left:
            n = min(left, 1 << 20)
            f.write(bytes(r.getrandbits(8) for _ in range(n)) if n < 4096 else r.randbytes(n) if hasattr(r, "randbytes") else os.urandom(n))
            left -= n


def make_file_fast(path, size, seed=1):
    """Deterministic, position-dependent content (each 8-byte word = seed:offset)."""
    import array
    with open(path, "wb") as f:
        off = 0
        while off < size:
            n = min(size - off, 1 << 20)
            words = (n + 7) // 8
            a = array.array("I", [0]) * (2 * words)
            a[0::2] = array.array("I", [seed & 0xFFFFFFFF]) * words
            a[1::2] = array.array("I", range(off // 8, off // 8 + words))
            f.write(a.tobytes()[:n])
            off += n


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def first_diff(a, b):
    with open(a, "rb") as fa, open(b, "rb") as fb:
        da, db = fa.read(), fb.read()
    if len(da) != len(db):
        n = min(len(da), len(db))
    else:
        n = len(da)
    for i in range(0, n, 4096):
        if da[i:i + 4096] != db[i:i + 4096]:
            for k in range(i, min(i + 4096, n)):
                if da[k] != db[k]:
                    return k
    return None if len(da) == len(db) else n


def run(args, cwd, timeout=60, env=None, exe=None):
    t0 = time.time()
    e = dict(os.environ)
    if env:
        e.update(env)
    p = subprocess.Popen([exe or EXE] + args, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL, env=e)
    try:
        out, _ = p.communicate(timeout=timeout)
        to = False
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        to = True
    return p.returncode, out.decode("utf-8", "replace"), time.time() - t0, to


def tail(out, n=6):
    lines = [l.split("\r")[-1] for l in out.replace("\x1b", "").splitlines() if l.strip()]
    return " | ".join(lines[-n:])


RESULTS = []
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass


def report(name, ok, detail):
    RESULTS.append((name, ok, detail))
    print("%-4s %-55s %s" % ("PASS" if ok else "FAIL", name, detail), flush=True)
