"""Long-path (> MAX_PATH) downloads, direct + HLS, with kill/resume.  python -I t_longpath.py"""
import os
import subprocess
import sys
import time
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa


def LP(p):
    return "\\\\?\\" + p


def main():
    d = fresh_dir("longpath")
    srv = os.path.join(d, "srv")
    os.makedirs(os.path.join(srv, "h"))
    make_file_fast(os.path.join(srv, "f.bin"), 6 * (1 << 20), 3)
    for i in range(4):
        make_file_fast(os.path.join(srv, "h", "s%d.ts" % i), 50000, i)
    open(os.path.join(srv, "h", "p.m3u8"), "w", newline="\n").write(
        "#EXTM3U\n" + "".join("#EXTINF:1,\ns%d.ts\n" % i for i in range(4)) + "#EXT-X-ENDLIST\n")
    out = os.path.join(d, *(["d" * 60] * 3))          # ~180 chars of folders below the run dir
    name = "N" * 170 + ".bin"
    S = Server(srv)
    try:
        cd = urllib.parse.quote('attachment; filename="%s"' % name)
        url = S.url("/f/f.bin?delay=0.05&cd=" + cd)
        p = subprocess.Popen([EXE, "-c", "4", "-d", out, url], cwd=d, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, stdin=subprocess.DEVNULL)
        time.sleep(2.5)
        p.kill()
        p.wait()
        rc, txt, el, to = run(["-c", "4", "-d", out, S.url("/f/f.bin?cd=" + cd)], d, 60)
        files = os.listdir(LP(out)) if os.path.isdir(LP(out)) else []
        full = len(os.path.join(out, name))
        ok = rc == 0 and files == [name] and sha(LP(os.path.join(out, name))) == sha(os.path.join(srv, "f.bin"))
        report("direct download to a %d-char path, killed + resumed" % full, ok,
               "rc=%s files=%s resumed=%s %s" % (rc, [f[:20] + "..." for f in files], "resuming" in txt, tail(txt, 1)[:120]))
        rc, txt, el, to = run(["-d", out, S.url("/f/h/p.m3u8")], d, 60)
        files = os.listdir(LP(out)) if os.path.isdir(LP(out)) else []
        report("HLS download to a long path", rc == 0 and any(f.endswith(".ts") for f in files),
               "rc=%s files=%s %s" % (rc, [f[:30] for f in files], tail(txt, 1)[:120]))
    finally:
        S.stop()


if __name__ == "__main__":
    main()
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
