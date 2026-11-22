"""yt-dlp bridge / process-launch tests.   python -I t_ytdlp.py [name-substring ...]
Needs adv\\harness\\fake_ytdlp2.exe (gcc -O1 -municode fake_ytdlp2.c -o fake_ytdlp2.exe)."""
import os
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

FAKE = os.path.join(ADV, "harness", "fake_ytdlp2.exe")


def setup(name):
    d = fresh_dir("yt_" + name)
    for sub in ("srv", "out", "bin", "tools", "cwd"):
        os.makedirs(os.path.join(d, sub))
    exe = os.path.join(d, "bin", "uloader.exe")       # private copy: nothing next to it
    shutil.copy(EXE, exe)
    return d, exe


def env_with_fake(d, out_text, extra=None):
    tools = os.path.join(d, "tools")
    shutil.copy(FAKE, os.path.join(tools, "yt-dlp.exe"))
    op = os.path.join(d, "ytout.txt")
    open(op, "wb").write(out_text.encode("utf-8"))
    e = {"PATH": tools + os.pathsep + os.environ["PATH"], "FAKE_YTDLP_OUT": op,
         "FAKE_YTDLP_LOG": os.path.join(d, "ytlog.txt")}
    if extra:
        e.update(extra)
    return e


def t_vext_traversal():
    d, exe = setup("vext")
    srv = os.path.join(d, "srv")
    make_file_fast(os.path.join(srv, "v.bin"), 3000, 1)
    make_file_fast(os.path.join(srv, "a.bin"), 2000, 2)
    S = Server(srv)
    try:
        txt = ulvid([S.url("/f/v.bin"), S.url("/f/a.bin")], vext="x\\..\\..\\..\\ESCAPED_VIDEO.bin",
                    aext="m4a", title="Title", vid="abc")
        out = os.path.join(d, "out")
        rc, o, el, to = run(["--ytdlp", "-d", out, "https://example.com/watch"], os.path.join(d, "cwd"), 60,
                            env=env_with_fake(d, txt), exe=exe)
        esc = [os.path.join(r, f) for r, _, fs in os.walk(RUNS) for f in fs if "ESCAPED" in f]
        report("yt-dlp video ext with ..\\ (vext not sanitized)", not any(not p.startswith(out) for p in esc),
               "rc=%s files outside -d: %s" % (rc, [p for p in esc if not p.startswith(out)]))
        for p in esc:
            os.remove(p)
    finally:
        S.stop()


def t_title_newline():
    d, exe = setup("titlenl")
    srv = os.path.join(d, "srv")
    make_file_fast(os.path.join(srv, "real.bin"), 3000, 1)
    make_file_fast(os.path.join(srv, "other.bin"), 3000, 2)
    S = Server(srv)
    try:
        # the title itself contains a line break and a link: it must stay part of the title
        txt = ulvid([S.url("/f/real.bin")], title="Cool clip\n%s" % S.url("/f/other.bin"), vid="abc")
        out = os.path.join(d, "out")
        rc, o, el, to = run(["--ytdlp", "-d", out, "https://example.com/watch"], os.path.join(d, "cwd"), 60,
                            env=env_with_fake(d, txt), exe=exe)
        reqs = [l.split()[1] for l in S.requests()]
        report("title containing a newline + URL", not any("other.bin" in r for r in reqs),
               "rc=%s server saw: %s; files=%s" % (rc, reqs, os.listdir(out)))
    finally:
        S.stop()


def t_cmdline_roundtrip():
    """URL / -f / headers with quotes, backslashes and spaces must reach yt-dlp unchanged"""
    d, exe = setup("cmdline")
    cases = {
        "url": 'https://youtube.com/watch?v=a"b\\" --exec calc \\\\"',
        "fmt": 'best[height<=720]" --exec "calc',
        "hdr": 'X-A: a\\"b c\\\\',
    }
    rc, o, el, to = run(["-f", cases["fmt"], "-H", cases["hdr"], cases["url"]], os.path.join(d, "cwd"), 60,
                        env=env_with_fake(d, ""), exe=exe)
    log = open(os.path.join(d, "ytlog.txt"), encoding="utf-8").read().splitlines()
    argv = [l.split("=", 1)[1] for l in log if l.startswith("ARGV[")]
    ok = cases["url"] in argv and cases["fmt"] in argv and cases["hdr"] in argv and "--exec" not in argv
    report("yt-dlp argv round trip (quotes/backslashes)", ok, "argv=%s" % argv[1:])


def t_planted_ytdlp():
    """a hostile server names its file yt-dlp.exe; the next video/HTML link runs it"""
    d, exe = setup("planted")
    srv = os.path.join(d, "srv")
    shutil.copy(FAKE, os.path.join(srv, "payload.exe"))
    S = Server(srv)
    try:
        import urllib.parse
        cwd = os.path.join(d, "cwd")
        u1 = S.url("/f/payload.exe?cd=" + urllib.parse.quote('attachment; filename="yt-dlp.exe"'))
        u2 = S.url("/html")
        e = {"PATH": os.environ["PATH"]}
        rc, o, el, to = run([u1, u2], cwd, 60, env=e, exe=exe)
        ran = os.path.exists(os.path.join(cwd, "yt-dlp.exe.ran"))
        report("downloaded yt-dlp.exe in cwd gets executed", not ran,
               "rc=%s files in cwd=%s ; %s" % (rc, sorted(os.listdir(cwd)), tail(o, 2)))
    finally:
        S.stop()


TESTS = [t_vext_traversal, t_title_newline, t_cmdline_roundtrip, t_planted_ytdlp]

if __name__ == "__main__":
    sel = sys.argv[1:]
    for t in TESTS:
        if sel and not any(s in t.__name__ for s in sel):
            continue
        try:
            t()
        except Exception as ex:
            import traceback
            traceback.print_exc()
            report(t.__name__, False, "harness exception: %r" % ex)
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
