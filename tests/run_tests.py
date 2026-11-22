r"""U-loader regression suite.

    python -I tests/run_tests.py [path\to\uloader.exe] [-k substring]

Starts tests/server.py on a free port, runs the downloader against it in
many good and bad situations, and checks the results byte for byte.
Needs only Python 3.8+ (stdlib). openssl (optional) enables the encrypted
HLS test, gcc (optional) enables the Ctrl+C test.
"""
import hashlib
import json
import os
import random
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import traceback
import urllib.parse

# a failure report may quote U-loader's progress bar: never crash on a
# console code page that cannot print it
for _s in (sys.stdout, sys.stderr):
    try:
        _s.reconfigure(errors="replace")
    except Exception:
        pass

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EXE = os.path.join(ROOT, "uloader.exe")
ONLY = None
args = sys.argv[1:]
while args:
    a = args.pop(0)
    if a == "-k":
        ONLY = args.pop(0)
    else:
        EXE = os.path.abspath(a)

WORK = tempfile.mkdtemp(prefix="uloader-tests-")
FIX = os.path.join(WORK, "fixture")
LOG = os.path.join(WORK, "requests.log")
os.makedirs(FIX)
ENV = dict(os.environ)
# keep the real yt-dlp / ffmpeg out of the way so results are predictable
ENV["PATH"] = os.pathsep.join(p for p in ENV.get("PATH", "").split(os.pathsep)
                              if not os.path.isfile(os.path.join(p, "yt-dlp.exe")))
# ... including the tools U-loader finds next to itself (the repo folder keeps
# a yt-dlp.exe): the tests run a private copy of the exe
_bin = os.path.join(WORK, "bin")
os.makedirs(_bin)
shutil.copy2(EXE, os.path.join(_bin, "uloader.exe"))
EXE_UNDER_TEST = EXE
EXE = os.path.join(_bin, "uloader.exe")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def make(name, size, seed=None):
    rnd = random.Random(seed if seed is not None else size)
    data = bytes(rnd.getrandbits(8) for _ in range(min(size, 65536)))
    with open(os.path.join(FIX, name), "wb") as f:
        left = size
        while left > 0:
            n = min(left, len(data))
            f.write(data[:n] if size <= 65536 else os.urandom(n))
            left -= n
    return os.path.join(FIX, name)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def run(argv, timeout=120, **kw):
    t0 = time.time()
    p = subprocess.run([EXE] + argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       timeout=timeout, env=ENV, **kw)
    out = p.stdout.decode("utf-8", "replace")
    return p.returncode, out, time.time() - t0


def requests_log():
    if not os.path.exists(LOG):
        return []
    with open(LOG, encoding="utf-8") as f:
        return [json.loads(l) for l in f if l.strip()]


def clear_log():
    if os.path.exists(LOG):
        os.remove(LOG)


def outdir(name):
    d = os.path.join(WORK, "out", name)
    os.makedirs(d, exist_ok=True)
    return d


class Fail(Exception):
    pass


def check(cond, msg):
    if not cond:
        raise Fail(msg)


def same(a, b, what="file"):
    check(os.path.exists(a), "%s missing: %s" % (what, a))
    check(sha(a) == sha(b), "%s differs from the original: %s" % (what, a))


def leftovers(d):
    return [f for f in os.listdir(d) if f.endswith((".ulpart", ".ulstate", ".tmp"))]


# ----------------------------------------------------------------------
PORT = free_port()
BASE = "http://127.0.0.1:%d" % PORT
F30 = make("big30.bin", 30 * 1024 * 1024)
F3 = make("three.bin", 3 * 1000 * 1000 + 7)
SIZES = {n: make("s%d.bin" % n, n) for n in (1, 1023, 1 << 20, (2 << 20) - 1, (2 << 20) + 1, 5 * 1024 * 1024 + 3)}
srv = subprocess.Popen([sys.executable, "-I", os.path.join(HERE, "server.py"), str(PORT), FIX, LOG],
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
for _ in range(50):
    try:
        socket.create_connection(("127.0.0.1", PORT), timeout=0.2).close()
        break
    except OSError:
        time.sleep(0.1)

TESTS = []


def test(fn):
    TESTS.append(fn)
    return fn


@test
def multi_connection_30mb():
    d = outdir("multi")
    rc, out, dt = run(["-c", "8", "-d", d, BASE + "/r/big30.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "big30.bin"), F30)
    check(not leftovers(d), "temp files left: %s" % leftovers(d))


@test
def edge_sizes():
    d = outdir("sizes")
    for n, src in SIZES.items():
        rc, out, _ = run(["-c", "8", "-d", d, BASE + "/r/s%d.bin" % n])
        check(rc == 0, "size %d: exit %d\n%s" % (n, rc, out))
        same(os.path.join(d, "s%d.bin" % n), src)


@test
def empty_file():
    d = outdir("empty")
    rc, out, _ = run(["-d", d, "-o", "empty.bin", BASE + "/empty"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    check(os.path.getsize(os.path.join(d, "empty.bin")) == 0, "not empty")


@test
def no_range_server():
    d = outdir("norange")
    rc, out, _ = run(["-c", "8", "-d", d, BASE + "/n/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "three.bin"), F3)


@test
def unknown_size_close_delimited():
    d = outdir("chunked")
    rc, out, _ = run(["-d", d, BASE + "/c/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "three.bin"), F3)


@test
def redirect_chain():
    d = outdir("redir")
    rc, out, _ = run(["-d", d, BASE + "/redir/5/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "three.bin"), F3)


@test
def redirect_loop_fails_fast():
    d = outdir("loop")
    rc, out, dt = run(["-d", d, "-R", "1", BASE + "/loop"], timeout=60)
    check(rc == 1, "expected failure, got %d" % rc)
    check("redirect" in out.lower(), "message should mention redirects:\n" + out)
    check(dt < 30, "took %.0fs" % dt)


@test
def cookie_not_leaked_across_hosts():
    d = outdir("xhost")
    clear_log()
    rc, out, _ = run(["-d", d, "-b", "sid=SECRET", BASE + "/xhost/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "three.bin"), F3)
    log = requests_log()
    first = [r for r in log if r["path"].startswith("/xhost/")]
    other = [r for r in log if r["host"].startswith("localhost")]
    check(first and first[0]["cookie"] == "sid=SECRET", "cookie not sent to its own host")
    check(other and all(r["cookie"] is None for r in other), "cookie leaked to another host: %s" % other)


@test
def cookies_file_scoping():
    d = outdir("jar")
    jar = os.path.join(WORK, "cookies.txt")
    with open(jar, "w") as f:
        f.write("# Netscape HTTP Cookie File\n")
        f.write("127.0.0.1\tFALSE\t/\tFALSE\t0\tgood\tyes\n")
        f.write("127.0.0.1\tFALSE\t/other\tFALSE\t0\twrongpath\tx\n")
        f.write(".example.com\tTRUE\t/\tFALSE\t0\tforeign\tx\n")
        f.write("127.0.0.1\tFALSE\t/\tTRUE\t0\tsecureonly\tx\n")
        f.write("127.0.0.1\tFALSE\t/\tFALSE\t1\texpired\tx\n")
    clear_log()
    rc, out, _ = run(["-d", d, "--cookies", jar, BASE + "/r/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    cookies = {r["cookie"] for r in requests_log()}
    check(cookies == {"good=yes"}, "wrong cookies sent: %s" % cookies)


@test
def connection_drops_are_survived():
    d = outdir("drop")
    rc, out, _ = run(["-c", "8", "-d", d, BASE + "/drop/big30.bin"], timeout=300)
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "big30.bin"), F30)


@test
def busy_server_503():
    d = outdir("busy")
    rc, out, _ = run(["-c", "4", "-d", d, BASE + "/busy/three.bin"], timeout=200)
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "three.bin"), F3)


@test
def hostile_filenames():
    d = outdir("names")
    cases = {
        "../../evil.txt": "evil.txt",
        "..\\..\\evil2.txt": "evil2.txt",
        "CON.txt": "_CON.txt",
        "a:b*c?.bin": "a_b_c_.bin",
        "   .hidden...   ": "hidden",
    }
    for given, expect in cases.items():
        rc, out, _ = run(["-d", d, BASE + "/cd/three.bin?name=" + __import__("urllib.parse").parse.quote(given)])
        check(rc == 0, "%r: exit %d\n%s" % (given, rc, out))
        check(os.path.exists(os.path.join(d, expect)), "%r should be saved as %r; got %s" % (given, expect, os.listdir(d)))
    outside = [f for f in os.listdir(os.path.join(WORK, "out")) if "evil" in f]
    check(not outside, "file escaped the download folder: %s" % outside)


@test
def unicode_filename():
    d = outdir("unicode")
    name = "বাংলা ভিডিও – test.bin"
    rc, out, _ = run(["-d", d, BASE + "/cdstar/three.bin?name=" + __import__("urllib.parse").parse.quote(name)])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, name), F3)


@test
def not_found_leaves_nothing():
    d = outdir("404")
    rc, out, _ = run(["-d", d, BASE + "/status/404"])
    check(rc == 1, "expected exit 1, got %d" % rc)
    check(os.listdir(d) == [], "files left behind: %s" % os.listdir(d))


@test
def html_page_is_not_saved():
    d = outdir("page")
    rc, out, _ = run(["-d", d, BASE + "/page"])
    check(rc == 1 and "web page" in out, "exit %d\n%s" % (rc, out))
    check(os.listdir(d) == [], "files left behind")


@test
def lying_content_length_fails_cleanly():
    d = outdir("lie")
    rc, out, dt = run(["-d", d, "-t", "5", "-R", "1", BASE + "/lie/three.bin"], timeout=120)
    check(rc == 1, "a short body must not count as success (exit %d)\n%s" % (rc, out))
    check(not os.path.exists(os.path.join(d, "three.bin")), "an incomplete file was saved as complete")


@test
def resume_after_hard_kill():
    d = outdir("kill")
    url = BASE + "/r/big30.bin?kbps=400"          # ~1.6 MB/s with 4 connections
    for attempt in range(3):
        p = subprocess.Popen([EXE, "-c", "4", "-d", d, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
        time.sleep(2.5 + attempt)
        p.kill()
        p.wait()
        check(os.path.exists(os.path.join(d, "big30.bin.ulstate")), "no resume state after kill #%d" % (attempt + 1))
    rc, out, _ = run(["-c", "4", "-d", d, BASE + "/r/big30.bin"])
    check(rc == 0 and "resuming" in out, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "big30.bin"), F30)


@test
def changed_file_restarts():
    d = outdir("changed")
    src = make("changing.bin", 8 * 1024 * 1024, seed=1)
    p = subprocess.Popen([EXE, "-c", "4", "-d", d, BASE + "/r/changing.bin?kbps=500"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=ENV)
    time.sleep(4)
    p.kill()
    p.wait()
    time.sleep(1.1)                      # new mtime -> new ETag
    with open(src, "r+b") as f:          # same size, different bytes
        f.write(os.urandom(1 << 20))
    rc, out, _ = run(["-c", "4", "-d", d, BASE + "/r/changing.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "changing.bin"), src)


@test
def ctrl_c_pause_and_resume():
    helper = os.path.join(WORK, "sendctrlc.exe")
    if shutil.which("gcc") is None:
        return "skipped (no gcc)"
    subprocess.run(["gcc", "-O2", os.path.join(HERE, "tools", "sendctrlc.c"), "-o", helper], check=True)
    d = outdir("ctrlc")
    p = subprocess.Popen([EXE, "-c", "4", "-d", d, BASE + "/r/big30.bin?kbps=1500"],
                         creationflags=subprocess.CREATE_NEW_CONSOLE, env=ENV)
    time.sleep(3)
    subprocess.run([helper, str(p.pid)])
    try:
        rc = p.wait(15)
    except subprocess.TimeoutExpired:
        p.kill()
        raise Fail("did not stop within 15 s of Ctrl+C")
    check(rc == 2, "paused download should exit with 2, got %d" % rc)
    check(os.path.exists(os.path.join(d, "big30.bin.ulstate")), "no state saved on pause")
    rc, out, _ = run(["-c", "4", "-d", d, BASE + "/r/big30.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "big30.bin"), F30)


@test
def queue_with_one_failure():
    d = outdir("queue")
    lst = os.path.join(WORK, "list.txt")
    with open(lst, "w", encoding="utf-8-sig") as f:
        f.write("# comment\n%s/r/three.bin\n\n%s/status/404\n%s/r/s1023.bin\n" % (BASE, BASE, BASE))
    rc, out, _ = run(["-d", d, "-i", lst])
    check(rc == 1, "a failed item must make the exit code 1 (got %d)" % rc)
    same(os.path.join(d, "three.bin"), F3)
    same(os.path.join(d, "s1023.bin"), SIZES[1023])


@test
def speed_limit():
    d = outdir("limit")
    rc, out, dt = run(["-c", "4", "-l", "1M", "-d", d, BASE + "/r/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    check(dt >= 2.3, "3 MB at 1 MB/s finished in %.1fs" % dt)
    same(os.path.join(d, "three.bin"), F3)


@test
def already_downloaded_is_skipped():
    d = outdir("again")
    run(["-d", d, BASE + "/r/three.bin"])
    rc, out, _ = run(["-d", d, BASE + "/r/three.bin"])
    check(rc == 0 and "Already downloaded" in out, out)
    check(sorted(os.listdir(d)) == ["three.bin"], "duplicate created: %s" % os.listdir(d))


@test
def file_is_dated_when_downloaded():
    """The server says 2019; the saved file must carry today's date (created and modified)."""
    d = outdir("dated")
    t0 = time.time()
    rc, out, _ = run(["-d", d, BASE + "/old/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    p = os.path.join(d, "three.bin")
    st = os.stat(p)
    check(abs(st.st_mtime - t0) < 120, "modified date is %s, not now" % time.ctime(st.st_mtime))
    check(abs(st.st_ctime - t0) < 120, "created date is %s, not now" % time.ctime(st.st_ctime))


@test
def already_downloaded_checks_the_version():
    """Re-run = skipped; a same-sized but different file on the server = downloaded again."""
    d = outdir("version")
    src = make("version.bin", 300000, seed=11)
    rc, out, _ = run(["-d", d, BASE + "/r/version.bin"])
    check(rc == 0, "first download: exit %d\n%s" % (rc, out))
    rc, out, _ = run(["-d", d, BASE + "/r/version.bin"])
    check(rc == 0 and "Already downloaded" in out, "re-run should be skipped:\n" + out)
    time.sleep(1.1)                                  # new mtime -> new ETag
    with open(src, "r+b") as f:
        f.write(os.urandom(1000))
    rc, out, _ = run(["-d", d, BASE + "/r/version.bin"])
    check(rc == 0 and "Already downloaded" not in out, "changed file must be downloaded again:\n" + out)
    same(os.path.join(d, "version (1).bin"), src)


@test
def http_trace_hides_secrets():
    """ULOADER_HTTP_TRACE logs the exchange but never cookie/authorization values."""
    d = outdir("trace")
    log = os.path.join(WORK, "http-trace.txt")
    env = dict(ENV, ULOADER_HTTP_TRACE=log)
    p = subprocess.run([EXE, "-d", d, "-b", "sid=TOPSECRET", "-H", "Authorization: Bearer ALSOSECRET",
                        BASE + "/r/three.bin"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, timeout=60)
    check(p.returncode == 0, "exit %d" % p.returncode)
    text = open(log, encoding="utf-8").read()
    check("GET /r/three.bin" in text and "< HTTP/1." in text, "trace incomplete:\n" + text[:500])
    check("Cookie: <" in text and "Authorization: <" in text, "secret headers not listed:\n" + text[:800])
    check("TOPSECRET" not in text and "ALSOSECRET" not in text, "a secret value leaked into the trace")


@test
def long_and_unicode_paths():
    d = os.path.join(outdir("long"), *(["very long folder name ঝ"] * 12))
    check(len(d) > 260, "path not long enough for the test")
    rc, out, _ = run(["-c", "4", "-d", d, BASE + "/r/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same("\\\\?\\" + os.path.join(d, "three.bin"), F3)


@test
def hls_aes128():
    if shutil.which("openssl") is None:
        return "skipped (no openssl)"
    hdir = os.path.join(FIX, "hls")
    os.makedirs(hdir, exist_ok=True)
    key = bytes(range(16))
    with open(os.path.join(FIX, "hls.key"), "wb") as f:
        f.write(key)
    plain = b""
    lines = ["#EXTM3U", "#EXT-X-TARGETDURATION:4", "#EXT-X-MEDIA-SEQUENCE:7",
             '#EXT-X-KEY:METHOD=AES-128,URI="/r/hls.key"']
    for i in range(6):
        seg = os.urandom(50000 + i * 777)
        plain += seg
        src = os.path.join(WORK, "seg%d.bin" % i)
        with open(src, "wb") as f:
            f.write(seg)
        subprocess.run(["openssl", "enc", "-aes-128-cbc", "-K", key.hex(), "-iv", "%032x" % (7 + i),
                        "-in", src, "-out", os.path.join(FIX, "hseg%d.ts" % i)], check=True)
        lines += ["#EXTINF:4.0,", "hseg%d.ts" % i]
    lines.append("#EXT-X-ENDLIST")
    with open(os.path.join(FIX, "aes.m3u8"), "w") as f:
        f.write("\n".join(lines) + "\n")
    expected = os.path.join(WORK, "hls-expected.bin")
    with open(expected, "wb") as f:
        f.write(plain)
    d = outdir("hls")
    rc, out, _ = run(["-d", d, BASE + "/r/aes.m3u8"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "aes.ts"), expected)


@test
def hls_names_and_cleanup():
    """'clip.mp4.m3u8' is saved as 'clip.ts' (not 'clip.mp4.ts'), no cache folder is left
    behind, and the same link again never overwrites the first file."""
    lines = ["#EXTM3U", "#EXT-X-TARGETDURATION:4"]
    plain = b""
    for i in range(4):
        data = os.urandom(40000 + i * 333)
        plain += data
        with open(os.path.join(FIX, "cseg%d.ts" % i), "wb") as f:
            f.write(data)
        lines += ["#EXTINF:4.0,", "cseg%d.ts" % i]
    lines.append("#EXT-X-ENDLIST")
    with open(os.path.join(FIX, "clip.mp4.m3u8"), "w") as f:
        f.write("\n".join(lines) + "\n")
    expected = os.path.join(WORK, "clip-expected.bin")
    with open(expected, "wb") as f:
        f.write(plain)
    d = outdir("hlsnames")
    rc, out, _ = run(["-d", d, BASE + "/r/clip.mp4.m3u8"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "clip.ts"), expected)
    check(not os.path.exists(os.path.join(d, "clip.ulhls")),
          "the segment cache folder was left behind: %s" % os.listdir(d))
    first = sha(os.path.join(d, "clip.ts"))
    with open(os.path.join(FIX, "cseg0.ts"), "wb") as f:      # the stream changed meanwhile
        f.write(os.urandom(40000))
    rc, out, _ = run(["-d", d, BASE + "/r/clip.mp4.m3u8"])
    check(rc == 0, "second run: exit %d\n%s" % (rc, out))
    check(sha(os.path.join(d, "clip.ts")) == first, "the first download was overwritten")
    check(os.path.exists(os.path.join(d, "clip (1).ts")),
          "the second run should be 'clip (1).ts': %s" % os.listdir(d))
    bad = [f for f in os.listdir(d) if f.endswith(".ulhls") or ".mp4." in f]
    check(not bad, "leftovers / double extensions: %s" % bad)


@test
def hls_same_name_at_the_same_time():
    """Two different streams with the same file name downloaded at the same time (a CDN
    that names every video '1080p.av1.mp4.m3u8'): each gets its own name and segment
    folder - the second must not delete the first one's segments ('disk full?' at the join)."""
    lines = ["#EXTM3U", "#EXT-X-TARGETDURATION:4"]
    plain = b""
    for i in range(6):
        data = os.urandom(40000 + i)
        plain += data
        with open(os.path.join(FIX, "dseg%d.ts" % i), "wb") as f:
            f.write(data)
        lines += ["#EXTINF:4.0,", "dseg%d.ts?kbps=100" % i]     # slow: the two runs overlap
    lines.append("#EXT-X-ENDLIST")
    with open(os.path.join(FIX, "dup.m3u8"), "w") as f:
        f.write("\n".join(lines) + "\n")
    expected = os.path.join(WORK, "dup-expected.bin")
    with open(expected, "wb") as f:
        f.write(plain)
    d = outdir("hlsdup")
    # two links -> two streams to U-loader (different playlist URLs), same name "dup"
    a = subprocess.Popen([EXE, "-c", "1", "-d", d, BASE + "/r/dup.m3u8"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=ENV)
    time.sleep(1.0)
    b = subprocess.Popen([EXE, "-c", "1", "-d", d, BASE + "/redir/1/dup.m3u8"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, env=ENV)
    out_a = a.communicate(timeout=120)[0].decode("utf-8", "replace")
    out_b = b.communicate(timeout=120)[0].decode("utf-8", "replace")
    check(a.returncode == 0, "first run: exit %d\n%s" % (a.returncode, out_a))
    check(b.returncode == 0, "second run: exit %d\n%s" % (b.returncode, out_b))
    same(os.path.join(d, "dup.ts"), expected, "first download")
    same(os.path.join(d, "dup (1).ts"), expected, "second download")
    check(not [f for f in os.listdir(d) if f.endswith((".ulhls", ".ulpart"))], "leftovers: %s" % os.listdir(d))


@test
def hls_wrong_key_fails():
    """A wrong AES-128 key fails the download instead of saving garbage as finished."""
    if shutil.which("openssl") is None:
        return "skipped (no openssl)"
    key = bytes(range(16))
    with open(os.path.join(FIX, "wrong.key"), "wb") as f:
        f.write(bytes(range(16, 32)))
    lines = ["#EXTM3U", "#EXT-X-TARGETDURATION:4",
             '#EXT-X-KEY:METHOD=AES-128,URI="/r/wrong.key",IV=0x00000000000000000000000000000001']
    for i in range(3):
        src = os.path.join(WORK, "wk%d.bin" % i)
        with open(src, "wb") as f:
            f.write(os.urandom(30000 + i))
        subprocess.run(["openssl", "enc", "-aes-128-cbc", "-K", key.hex(), "-iv", "%032x" % 1,
                        "-in", src, "-out", os.path.join(FIX, "wkseg%d.ts" % i)], check=True)
        lines += ["#EXTINF:4.0,", "wkseg%d.ts" % i]
    lines.append("#EXT-X-ENDLIST")
    with open(os.path.join(FIX, "wrongkey.m3u8"), "w") as f:
        f.write("\n".join(lines) + "\n")
    d = outdir("hlswrongkey")
    rc, out, _ = run(["-d", d, BASE + "/r/wrongkey.m3u8"])
    check(rc != 0, "a wrong key must fail the download\n%s" % out)
    check("decryption failed" in out, "no decryption error reported:\n%s" % out)
    check(not os.path.exists(os.path.join(d, "wrongkey.ts")), "garbage was saved as the result")


@test
def disposition_parameter_names_are_exact():
    """'xfilename=' and a 'filename=' inside another quoted value are not the file name."""
    d = outdir("cdexact")
    h = 'attachment; xfilename="evil.exe"; title="a filename=bad.exe b"; filename="good.bin"'
    rc, out, _ = run(["-d", d, BASE + "/cdraw/three.bin?h=" + urllib.parse.quote(h)])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "good.bin"), F3)
    check(os.listdir(d) == ["good.bin"], "unexpected files: %s" % os.listdir(d))


@test
def planted_tool_in_download_folder_is_never_run():
    """A downloaded 'yt-dlp.exe' sitting in the current folder must not be executed."""
    if shutil.which("gcc") is None:
        return "skipped (no gcc)"
    d = outdir("planted")
    exe = os.path.join(d, "yt-dlp.exe")
    subprocess.run(["gcc", "-O2", os.path.join(HERE, "tools", "marker.c"), "-o", exe], check=True)
    shutil.copy(exe, os.path.join(d, "ffmpeg.exe"))
    rc, out, _ = run([BASE + "/page"], cwd=d)            # a web page -> would look for yt-dlp
    check(not os.path.exists(exe + ".ran"), "the planted yt-dlp.exe was executed!")
    check("web page" in out, "expected the 'web page' message:\n" + out)


@test
def stale_state_after_no_range_run():
    """ranges -> killed; server stops supporting ranges -> killed; ranges again."""
    d = outdir("stale")
    src = make("toggle.bin", 6 * 1024 * 1024, seed=7)
    url = BASE + "/toggle/toggle.bin?kbps=300"
    flag = os.path.join(FIX, "_norange")
    try:
        for norange in (False, True):
            if norange:
                open(flag, "w").close()
            p = subprocess.Popen([EXE, "-c", "4", "-d", d, url], stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, env=ENV)
            time.sleep(3)
            p.kill()
            p.wait()
    finally:
        if os.path.exists(flag):
            os.remove(flag)
    rc, out, _ = run(["-c", "4", "-d", d, BASE + "/toggle/toggle.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "toggle.bin"), src)


@test
def pause_while_connection_stalled():
    """Ctrl+C while some connections hang must stop quickly and cleanly."""
    helper = os.path.join(WORK, "sendctrlc.exe")
    if shutil.which("gcc") is None:
        return "skipped (no gcc)"
    if not os.path.exists(helper):
        subprocess.run(["gcc", "-O2", os.path.join(HERE, "tools", "sendctrlc.c"), "-o", helper], check=True)
    d = outdir("stall")
    p = subprocess.Popen([EXE, "-c", "4", "-t", "60", "-d", d, BASE + "/stall/big30.bin"],
                         creationflags=subprocess.CREATE_NEW_CONSOLE, env=ENV)
    time.sleep(4)
    t0 = time.time()
    subprocess.run([helper, str(p.pid)])
    try:
        rc = p.wait(20)
    except subprocess.TimeoutExpired:
        p.kill()
        raise Fail("still running 20 s after Ctrl+C (blocked on a stalled connection)")
    dt = time.time() - t0
    check(rc == 2, "expected exit 2 (paused), got %d (0x%08x)" % (rc, rc & 0xFFFFFFFF))
    check(dt < 8, "pausing took %.1f s" % dt)
    check(os.path.exists(os.path.join(d, "big30.bin.ulstate")), "no state saved")


@test
def credentials_not_forwarded_across_hosts():
    d = outdir("auth")
    clear_log()
    rc, out, _ = run(["-d", d, "-H", "Authorization: Bearer SECRET", "-H", "X-Keep: yes",
                      BASE + "/xhost/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    log = requests_log()
    first = [r for r in log if r["path"].startswith("/xhost/")]
    other = [r for r in log if r["host"].startswith("localhost")]
    check(first and first[0]["auth"] == "Bearer SECRET", "Authorization not sent to its own host")
    check(other and all(r["auth"] is None for r in other), "Authorization leaked to another host")


@test
def site_cookie_carries_within_a_download():
    """A cookie the server sets on a redirect is sent with every later request of that
    download (all connections); one it deletes (Max-Age=0) is not."""
    d = outdir("sitecookie")
    clear_log()
    rc, out, _ = run(["-d", d, "-c", "4", BASE + "/setcookie/three.bin"])
    check(rc == 0, "exit %d\n%s" % (rc, out))
    same(os.path.join(d, "three.bin"), F3)
    gated = [r for r in requests_log() if r["path"].startswith("/needcookie/")]
    check(len(gated) >= 2, "expected several requests to the gated file, saw %d" % len(gated))
    check(all("ulgate=open" in (r["cookie"] or "") for r in gated), "the site cookie was not sent every time")
    check(not any("ulgone" in (r["cookie"] or "") for r in gated), "a deleted cookie was sent")


@test
def empty_input_list_is_an_error():
    lst = os.path.join(WORK, "empty-list.txt")
    with open(lst, "w") as f:
        f.write("# nothing here\n\n")
    rc, out, _ = run(["-i", lst], timeout=20, stdin=subprocess.DEVNULL)
    check(rc == 1 and "no links" in out, "exit %d\n%s" % (rc, out))


@test
def native_host_protocol():
    """Talk to uloader.exe the way Chrome does (4-byte length + JSON)."""
    import struct

    def exchange(payloads):
        p = subprocess.Popen([EXE, "chrome-extension://ednpijjpophhlcbidanppgjliblinjga/"],
                             stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=ENV)
        data = b"".join(struct.pack("<I", len(x)) + x for x in payloads)
        out, _ = p.communicate(data, timeout=30)
        res = []
        while len(out) >= 4:
            n = struct.unpack("<I", out[:4])[0]
            res.append(json.loads(out[4:4 + n]))
            out = out[4 + n:]
        return p.returncode, res

    rc, res = exchange([b'{"action":"ping"}', b'{"action":"nope"}', b'not json',
                        json.dumps({"action": "download", "url": "javascript:alert(1)"}).encode()])
    check(res[0].get("ok") is True and "version" in res[0], "ping failed: %s" % res)
    check(res[1].get("ok") is False and res[2].get("ok") is False, "bad requests must be refused: %s" % res)
    check(res[3].get("ok") is False, "javascript: link must be refused: %s" % res)
    # verify: a captured browser download is only accepted when U-loader can
    # really get the file (refusals are answered before any job is created)
    rc, res = exchange([json.dumps({"action": "download", "url": BASE + "/status/403", "verify": True}).encode(),
                        json.dumps({"action": "download", "url": BASE + "/page", "filename": "report.pdf",
                                    "verify": True}).encode()])
    check(res[0].get("ok") is False and res[0].get("status") == 403, "403 must be refused: %s" % res[0])
    check(res[1].get("ok") is False and "web page" in res[1].get("error", ""), "a login page must be refused: %s" % res[1])
    # an oversized length prefix must not crash or hang the host
    p = subprocess.Popen([EXE, "chrome-extension://x/"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, env=ENV)
    p.communicate(struct.pack("<I", 0x7FFFFFFF) + b"{}", timeout=10)
    check(p.returncode is not None, "host hung on a huge length")


# ----------------------------------------------------------------------
def main():
    print("U-loader tests: %s (run as a private copy)\nwork dir: %s\n" % (EXE_UNDER_TEST, WORK))
    passed = failed = skipped = 0
    for fn in TESTS:
        if ONLY and ONLY not in fn.__name__:
            continue
        t0 = time.time()
        try:
            note = fn()
            if isinstance(note, str) and note.startswith("skipped"):
                skipped += 1
                print("  SKIP  %-36s %s" % (fn.__name__, note))
            else:
                passed += 1
                print("  PASS  %-36s %5.1fs" % (fn.__name__, time.time() - t0))
        except Exception as e:  # noqa: BLE001 - report everything
            failed += 1
            msg = str(e) if isinstance(e, Fail) else traceback.format_exc()
            print("  FAIL  %-36s %5.1fs\n        %s" % (fn.__name__, time.time() - t0,
                                                       msg.strip().replace("\n", "\n        ")[:3000]))
        sys.stdout.flush()
    srv.kill()
    print("\n%d passed, %d failed, %d skipped" % (passed, failed, skipped))
    if not failed:
        shutil.rmtree(WORK, ignore_errors=True)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
