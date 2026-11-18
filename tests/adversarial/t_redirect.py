"""Redirect / cookie-scoping tests for the v2 HTTP layer.  python -I t_redirect.py
Two servers: A on 127.0.0.1, B reached as "localhost" (a different host name)."""
import os
import shutil
import sys
import urllib.parse

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

FAKE = os.path.join(ADV, "harness", "fake_ytdlp2.exe")


def hdr_lines(S):
    return [l for l in S.requests() if l.startswith("HEADERS")]


def main():
    d = fresh_dir("redirect")
    a_root, b_root, out = os.path.join(d, "a"), os.path.join(d, "b"), os.path.join(d, "out")
    for x in (a_root, b_root, out):
        os.makedirs(x)
    A, B = Server(a_root), Server(b_root)
    b_url = "http://localhost:%d/hdr/file.bin" % B.port
    try:
        # 1. jar cookie for 127.0.0.1 must not follow a redirect to another host
        ck = os.path.join(d, "cookies.txt")
        open(ck, "w").write("# Netscape HTTP Cookie File\n127.0.0.1\tFALSE\t/\tFALSE\t0\tsid\tJARSECRET\n")
        rc, txt, el, to = run(["--cookies", ck, "-d", out, "--overwrite", A.url("/redir?to=" + urllib.parse.quote(b_url))], out, 30)
        leak = [l for l in hdr_lines(B) if "JARSECRET" in l]
        report("cookie jar entry follows cross-host redirect", not leak, "rc=%s leaked=%s" % (rc, leak[:1]))

        # 2. -b raw cookie (scoped to the link's host) across a redirect
        rc, txt, el, to = run(["-b", "raw=RAWSECRET", "-d", out, "--overwrite", A.url("/redir?to=" + urllib.parse.quote(b_url))], out, 30)
        leak = [l for l in hdr_lines(B) if "RAWSECRET" in l]
        report("-b cookie follows cross-host redirect", not leak, "rc=%s leaked=%s" % (rc, leak[:1]))

        # 3. -H Authorization across a cross-host redirect
        rc, txt, el, to = run(["-H", "Authorization: Bearer AUTHSECRET", "-d", out, "--overwrite",
                               A.url("/redir?to=" + urllib.parse.quote(b_url))], out, 30)
        leak = [l for l in hdr_lines(B) if "AUTHSECRET" in l]
        report("-H Authorization follows cross-host redirect", not leak, "rc=%s leaked=%s" % (rc, [l[:160] for l in leak[:1]]))

        # 4. yt-dlp http_headers containing a Cookie, media URL redirects to another host
        tools = os.path.join(d, "tools")
        os.makedirs(tools)
        shutil.copy(FAKE, os.path.join(tools, "yt-dlp.exe"))
        ytout = os.path.join(d, "ytout.txt")
        media = A.url("/redir?to=" + urllib.parse.quote(b_url))
        open(ytout, "wb").write(("ULVID\tbin\t\t\t{\"Cookie\": \"YTSECRET=1\", \"User-Agent\": \"x\"}\tclip [id]\n%s\n" % media).encode())
        env = {"PATH": tools + os.pathsep + os.environ["PATH"], "FAKE_YTDLP_OUT": ytout}
        rc, txt, el, to = run(["--ytdlp", "-d", out, "--overwrite", "https://video.example/watch?v=1"], out, 60, env=env)
        leak = [l for l in hdr_lines(B) if "YTSECRET" in l]
        report("yt-dlp Cookie header follows cross-host redirect", not leak, "rc=%s leaked=%s" % (rc, [l[:160] for l in leak[:1]]))

        # 5. redirect targets with other schemes / odd Locations
        odd = {"file": "file:///C:/Windows/win.ini", "empty": "", "javascript": "javascript:alert(1)",
               "ftp-closed-port": "ftp://127.0.0.1:1/x.bin", "spaces": "   ",
               "scheme-relative": "//localhost:%d/hdr/sr.bin" % B.port, "backslash": "\\\\localhost:%d\\hdr\\bs.bin" % B.port,
               "data": "data:text/plain,hello"}
        for name, loc in odd.items():
            rc, txt, el, to = run(["-R", "1", "-d", out, "--overwrite", A.url("/redir?to=" + urllib.parse.quote(loc))], out, 60)
            report("redirect to %s" % name, not to, "rc=%s %.1fs %s" % (rc, el, tail(txt, 1)[:150]))
    finally:
        A.stop()
        B.stop()


if __name__ == "__main__":
    main()
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
