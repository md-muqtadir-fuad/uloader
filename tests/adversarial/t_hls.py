"""HLS (hls.c / http_get_body) adversarial tests.   python -I t_hls.py [name-substring ...]"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

KiB = 1024


def setup(name):
    d = fresh_dir("hls_" + name)
    srv = os.path.join(d, "srv")
    out = os.path.join(d, "out")
    os.makedirs(srv)
    os.makedirs(out)
    return d, srv, out


def w(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w", newline="\n" if isinstance(data, str) else None) as f:
        f.write(data)


def blob(n, seed):
    p = os.path.join(RUNS, "_tmpblob")
    make_file_fast(p, n, seed)
    b = open(p, "rb").read()
    os.remove(p)
    return b


def media_pl(names, extra=""):
    s = "#EXTM3U\n#EXT-X-VERSION:3\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:0\n" + extra
    for n in names:
        s += "#EXTINF:2.0,\n%s\n" % n
    return s + "#EXT-X-ENDLIST\n"


# ---------------------------------------------------------------------------
def t_byterange_no_range_server():
    d, srv, out = setup("byterange")
    media = blob(3000, 5)
    w(os.path.join(srv, "media.ts"), media)
    pl = ("#EXTM3U\n#EXT-X-VERSION:4\n#EXT-X-TARGETDURATION:1\n"
          "#EXTINF:1,\n#EXT-X-BYTERANGE:1000@0\nmedia.ts\n"
          "#EXTINF:1,\n#EXT-X-BYTERANGE:1000\nmedia.ts\n"
          "#EXTINF:1,\n#EXT-X-BYTERANGE:1000\nmedia.ts\n#EXT-X-ENDLIST\n")
    w(os.path.join(srv, "show", "x.m3u8"), pl)
    os.rename(os.path.join(srv, "media.ts"), os.path.join(srv, "show", "media.ts"))
    w(os.path.join(srv, "_opts"), "ranges=0")          # server ignores Range (plain static server)
    S = Server(srv)
    try:
        rc, txt, el, to = run(["-d", out, S.url("/f/show/x.m3u8")], out, 60)
        files = [f for f in os.listdir(out)]
        o = os.path.join(out, files[0]) if files else None
        got = open(o, "rb").read() if o else b""
        report("BYTERANGE playlist, server ignores Range", rc != 0 or got == media,
               "rc=%s out=%s size=%d expected=%d" % (rc, files, len(got), len(media)))
    finally:
        S.stop()


def t_variant_mix():
    """pause a -q 360 download, run again without -q: segments of both qualities get joined"""
    d, srv, out = setup("variantmix")
    n = 24
    for v, seed in (("lo", 1), ("hi", 2)):
        for i in range(n):
            w(os.path.join(srv, "movie", v, "s%02d.ts" % i), blob(200 * KiB, seed * 1000 + i))
        w(os.path.join(srv, "movie", v, "index.m3u8"), media_pl(["s%02d.ts" % i for i in range(n)]))
    master = ("#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=500000,RESOLUTION=640x360\nlo/index.m3u8\n"
              "#EXT-X-STREAM-INF:BANDWIDTH=5000000,RESOLUTION=1920x1080\nhi/index.m3u8\n")
    w(os.path.join(srv, "movie", "master.m3u8"), master)
    w(os.path.join(srv, "_opts"), "delay=0.4")
    S = Server(srv)
    try:
        url = S.url("/f/movie/master.m3u8")
        p = subprocess.Popen([EXE, "-c", "4", "-q", "360", "-d", out, url], cwd=out, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        time.sleep(4)
        p.kill()
        p.wait()
        cache = [f for f in os.listdir(out)]
        os.remove(os.path.join(srv, "_opts"))
        rc, txt, el, to = run(["-c", "4", "-d", out, url], out, 120)
        outs = [f for f in os.listdir(out) if f.endswith(".ts")]
        if not outs:
            report("pause at 360p, resume at 1080p", False, "no output; rc=%s %s" % (rc, tail(txt, 2)))
            return
        got = open(os.path.join(out, outs[0]), "rb").read()
        hi = b"".join(open(os.path.join(srv, "movie", "hi", "s%02d.ts" % i), "rb").read() for i in range(n))
        lo = b"".join(open(os.path.join(srv, "movie", "lo", "s%02d.ts" % i), "rb").read() for i in range(n))
        seg = 200 * KiB
        src = ["lo" if got[i * seg:(i + 1) * seg] == lo[i * seg:(i + 1) * seg] else
               "hi" if got[i * seg:(i + 1) * seg] == hi[i * seg:(i + 1) * seg] else "??" for i in range(n)]
        report("pause at 360p, resume at 1080p", got == hi or rc != 0,
               "cache after pause=%s rc=%s segment sources=%s" % (cache, rc, "".join(x[0] for x in src)))
    finally:
        S.stop()


def t_aes():
    import binascii
    d, srv, out = setup("aes")
    key = bytes(range(16))
    w(os.path.join(srv, "k", "key.bin"), key)
    plain = []
    n = 6
    for i in range(n):
        pt = blob(1000 * (i + 1) + (16 if i == 2 else 0) - (1000 * (i + 1)) % 16 if i in (2, 3) else 1000 * (i + 1) + 7, 50 + i)
        plain.append(pt)
        w(os.path.join(srv, "k", "p%d.bin" % i), pt)
        iv = "%032x" % (i + 10) if i % 2 == 0 else None    # odd segments: IV = media sequence number
        ivhex = iv if iv else "%032x" % (100 + i)
        subprocess.check_call(["openssl", "enc", "-aes-128-cbc", "-K", binascii.hexlify(key).decode(), "-iv", ivhex,
                               "-in", os.path.join(srv, "k", "p%d.bin" % i), "-out", os.path.join(srv, "k", "s%d.ts" % i)])
    pl = "#EXTM3U\n#EXT-X-TARGETDURATION:2\n#EXT-X-MEDIA-SEQUENCE:100\n"
    for i in range(n):
        if i % 2 == 0:
            pl += '#EXT-X-KEY:METHOD=AES-128,URI="key.bin",IV=0x%032x\n' % (i + 10)
        else:
            pl += '#EXT-X-KEY:METHOD=AES-128,URI="key.bin"\n'
        pl += "#EXTINF:2,\ns%d.ts\n" % i
    pl += "#EXT-X-ENDLIST\n"
    w(os.path.join(srv, "k", "enc.m3u8"), pl)
    S = Server(srv)
    try:
        rc, txt, el, to = run(["-d", out, S.url("/f/k/enc.m3u8")], out, 60)
        outs = [f for f in os.listdir(out) if not f.endswith(".ulhls")]
        got = open(os.path.join(out, outs[0]), "rb").read() if outs else b""
        report("AES-128 with explicit IV and sequence IV", rc == 0 and got == b"".join(plain),
               "rc=%s out=%s match=%s %s" % (rc, outs, got == b"".join(plain), tail(txt, 1)))
    finally:
        S.stop()


def t_truncated_playlist():
    d, srv, out = setup("truncpl")
    n = 10
    for i in range(n):
        w(os.path.join(srv, "t", "s%d.ts" % i), blob(10000, i))
    pl = media_pl(["s%d.ts" % i for i in range(n)])
    w(os.path.join(srv, "t", "full.m3u8"), pl)
    cut = pl.index("s5.ts")
    w(os.path.join(srv, "t", "list.m3u8"), pl[:cut])     # served with Content-Length of the full list
    S = Server(srv)
    try:
        rc, txt, el, to = run(["-R", "1", "-d", out, S.url("/f/t/list.m3u8?liecl=%d" % len(pl))], out, 60)
        outs = [f for f in os.listdir(out) if not f.endswith(".ulhls")]
        size = os.path.getsize(os.path.join(out, outs[0])) if outs else 0
        report("playlist body shorter than its Content-Length", rc != 0,
               "rc=%s saved %d of %d bytes; %s" % (rc, size, n * 10000, tail(txt, 3)))
    finally:
        S.stop()


def mem_of(pid):
    class PMC(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD), ("PeakWorkingSetSize", ctypes.c_size_t),
                    ("WorkingSetSize", ctypes.c_size_t), ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaPagedPoolUsage", ctypes.c_size_t), ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
                    ("QuotaNonPagedPoolUsage", ctypes.c_size_t), ("PagefileUsage", ctypes.c_size_t),
                    ("PeakPagefileUsage", ctypes.c_size_t)]
    k = ctypes.windll.kernel32
    h = k.OpenProcess(0x1000 | 0x0010, False, pid)
    c = PMC()
    c.cb = ctypes.sizeof(c)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb)
    k.CloseHandle(h)
    return c.PagefileUsage


def t_infinite_playlist():
    d, srv, out = setup("infpl")
    S = Server(srv)
    try:
        p = subprocess.Popen([EXE, "-d", out, S.url("/inf?x=.m3u8")], cwd=out, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        samples = []
        for _ in range(8):
            time.sleep(1)
            if p.poll() is not None:
                break
            samples.append(mem_of(p.pid) // (1 << 20))
        alive = p.poll() is None
        p.kill()
        p.wait()
        report("endless playlist body (chunked, never ends)", not (alive and samples[-1] > 500),
               "still running after 8 s=%s, private MB per second: %s" % (alive, samples))
    finally:
        S.stop()


def t_hls_speed_limit():
    d, srv, out = setup("hlslimit")
    n = 8
    for i in range(n):
        w(os.path.join(srv, "l", "s%d.ts" % i), blob(256 * KiB, i))
    w(os.path.join(srv, "l", "x.m3u8"), media_pl(["s%d.ts" % i for i in range(n)]))
    S = Server(srv)
    try:
        rc, txt, el, to = run(["-l", "200K", "-d", out, S.url("/f/l/x.m3u8")], out, 60)
        report("-l 200K honoured for HLS (2 MB => >= ~9 s)", el >= 8, "rc=%s took %.1fs" % (rc, el))
    finally:
        S.stop()


def t_nested_master():
    d, srv, out = setup("nested")
    for i in range(3):
        w(os.path.join(srv, "n", "s%d.ts" % i), blob(5000, i))
    w(os.path.join(srv, "n", "media.m3u8"), media_pl(["s%d.ts" % i for i in range(3)]))
    w(os.path.join(srv, "n", "inner.m3u8"), "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\nmedia.m3u8\n")
    w(os.path.join(srv, "n", "outer.m3u8"), "#EXTM3U\n#EXT-X-STREAM-INF:BANDWIDTH=1\ninner.m3u8\n")
    S = Server(srv)
    try:
        rc, txt, el, to = run(["-d", out, S.url("/f/n/outer.m3u8")], out, 60)
        outs = [f for f in os.listdir(out)]
        got = open(os.path.join(out, outs[0]), "rb").read() if outs else b""
        report("master -> master -> media", rc != 0 or got.startswith(open(os.path.join(srv, "n", "s0.ts"), "rb").read()[:100]),
               "rc=%s out=%s first bytes=%r" % (rc, outs, got[:40]))
    finally:
        S.stop()


TESTS = [t_byterange_no_range_server, t_variant_mix, t_aes, t_truncated_playlist, t_infinite_playlist,
         t_hls_speed_limit, t_nested_master]

if __name__ == "__main__":
    sel = sys.argv[1:]
    for t in TESTS:
        if sel and not any(s in t.__name__ for s in sel):
            continue
        try:
            t()
        except Exception as e:
            import traceback
            traceback.print_exc()
            report(t.__name__, False, "harness exception: %r" % e)
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
