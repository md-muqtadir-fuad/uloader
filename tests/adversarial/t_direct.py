"""Direct-download (engine.c / http.c / main.c) adversarial tests.

python -I t_direct.py [test-name-substring ...]
env ULOADER_EXE overrides the binary (default review-v1\\uloader.exe).
"""
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

MiB = 1 << 20


def setup(name, files):
    d = fresh_dir(name)
    srv = os.path.join(d, "srv")
    out = os.path.join(d, "out")
    os.makedirs(srv)
    os.makedirs(out)
    for fn, (size, seed) in files.items():
        make_file_fast(os.path.join(srv, fn), size, seed)
    return d, srv, out


def kill_after(args, cwd, seconds):
    """start uloader, hard-kill it (TerminateProcess) after `seconds`"""
    p = subprocess.Popen([EXE] + args, cwd=cwd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                         stdin=subprocess.DEVNULL)
    try:
        p.wait(seconds)
        return p.returncode
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()
        return None


# ---------------------------------------------------------------------------
def t_sizes():
    sizes = [0, 1, 2, MiB - 1, MiB, MiB + 1, 2 * MiB - 1, 2 * MiB, 2 * MiB + 1, 3 * MiB, 4 * MiB - 1,
             4 * MiB + 1, 8 * MiB + 7, 17 * MiB + 3]
    d, srv, out = setup("sizes", {"s%d.bin" % s: (s, s + 1) for s in sizes})
    S = Server(srv)
    try:
        for mode in ("", "?ranges=0", "?chunked=1"):
            bad = []
            for s in sizes:
                fn = "s%d.bin" % s
                o = os.path.join(out, "m%s_%s" % (mode.strip("?=").replace("=", ""), fn))
                rc, txt, el, to = run(["-c", "8", "-o", o, S.url("/f/" + fn + mode)], out, 60)
                ok = (not to and rc == 0 and os.path.exists(o) and sha(o) == sha(os.path.join(srv, fn)))
                if not ok:
                    bad.append("%d(rc=%s,to=%s,%s)" % (s, rc, to, tail(txt, 2)))
            report("sizes%s" % mode, not bad, "; ".join(bad) or "%d sizes ok" % len(sizes))
    finally:
        S.stop()


def t_second_conn_200():
    """probe gets 206, every other range request gets 200 full body"""
    d, srv, out = setup("r206first", {"a.bin": (12 * MiB, 7)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        rc, txt, el, to = run(["-c", "8", "-R", "2", "-o", o, S.url("/f/a.bin?r206first=1&delay=0.01")], out, 120)
        ok = rc == 0 and os.path.exists(o) and sha(o) == sha(os.path.join(srv, "a.bin"))
        report("206 probe, 200 on other connections", ok, "rc=%s to=%s %.1fs: %s" % (rc, to, el, tail(txt, 3)))
    finally:
        S.stop()


def t_probe_lies_start():
    d, srv, out = setup("probestart", {"a.bin": (3 * MiB, 9)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        rc, txt, el, to = run(["-c", "1", "-o", o, S.url("/f/a.bin?probestart=1000")], out, 60)
        same = os.path.exists(o) and sha(o) == sha(os.path.join(srv, "a.bin"))
        report("probe Content-Range start lies (1000 instead of 0)", not (rc == 0 and not same),
               "rc=%s saved=%s identical=%s first_diff=%s" % (rc, os.path.exists(o), same,
                                                             first_diff(o, os.path.join(srv, "a.bin")) if os.path.exists(o) else None))
    finally:
        S.stop()


def t_lying_content_length():
    """200, no ranges, Content-Length claims 2 MiB, only 1 MiB sent, then close"""
    d, srv, out = setup("liecl", {"a.bin": (MiB, 3)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        rc, txt, el, to = run(["-R", "3", "-o", o, S.url("/f/a.bin?liecl=%d" % (2 * MiB))], out, 90)
        nreq = len([l for l in S.requests() if "/f/a.bin" in l])
        report("Content-Length larger than body, -R 3 (must fail, not loop)", not to,
               "timed_out=%s after %.0fs, rc=%s, %d requests made" % (to, el, rc, nreq))
    finally:
        S.stop()


def t_change_mid_download(etag):
    """file replaced on the server (same size) after 3 MiB were sent; every response is
    cut after 1 MiB so the workers must re-request (as on a flaky line)"""
    name = "swap_etag" if etag else "swap_noetag"
    d, srv, out = setup(name, {"a.bin": (16 * MiB, 1), "b.bin": (16 * MiB, 2)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        q = "swap=b.bin&swapafter=%d&delay=0.02&dropafter=%d" % (3 * MiB, MiB)
        if etag:
            q += "&etag=v1&swapetag=v2"
        rc, txt, el, to = run(["-c", "8", "-o", o, S.url("/f/a.bin?" + q)], out, 120)
        a_old = os.path.join(d, "old.bin")
        make_file_fast(a_old, 16 * MiB, 1)
        b_new = os.path.join(srv, "b.bin")
        if os.path.exists(o):
            h = sha(o)
            verdict = "== old file" if h == sha(a_old) else "== new file" if h == sha(b_new) else "MIXED old/new"
        else:
            verdict = "no output"
        ok = not (rc == 0 and verdict == "MIXED old/new")
        report("file changes mid-download (%s)" % ("ETag v1->v2" if etag else "no ETag"), ok,
               "rc=%s result %s: %s" % (rc, verdict, tail(txt, 1)))
    finally:
        S.stop()


def t_resume_changed(kind):
    """kill mid-download, replace the file (same size), resume"""
    d, srv, out = setup("resume_" + kind, {"a.bin": (16 * MiB, 1)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        opts = {"noetag": "delay=0.15", "lastmod": "delay=0.15&lm=Mon, 01 Jan 2024 00:00:00 GMT",
                "etag": "delay=0.15&etag=v1"}[kind]
        open(os.path.join(srv, "_opts"), "w").write(opts)
        url = S.url("/f/a.bin")
        kill_after(["-c", "4", "-o", o, url], out, 3.5)
        st = o + ".ulstate"
        have_state = os.path.exists(st)
        make_file_fast(os.path.join(srv, "a.bin"), 16 * MiB, 2)        # new content, same size
        opts2 = {"noetag": "", "lastmod": "lm=Tue, 02 Jan 2024 00:00:00 GMT", "etag": "etag=v2"}[kind]
        open(os.path.join(srv, "_opts"), "w").write(opts2)
        rc, txt, el, to = run(["-c", "4", "-o", o, url], out, 120)
        h = sha(o) if os.path.exists(o) else None
        new = sha(os.path.join(srv, "a.bin"))
        ok = rc != 0 or h == new
        report("resume after server file changed (%s)" % kind, ok,
               "state_saved=%s rc=%s output==new:%s, msg: %s" % (have_state, rc, h == new, tail(txt, 3)))
    finally:
        S.stop()


def t_stale_state_after_200():
    """run1 (ranges) killed -> run2: server answers 200 (no ranges), killed -> run3 ranges again"""
    d, srv, out = setup("stalestate", {"a.bin": (16 * MiB, 5)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        url = S.url("/f/a.bin")
        open(os.path.join(srv, "_opts"), "w").write("delay=0.15&etag=v1")
        kill_after(["-c", "8", "-o", o, url], out, 3.5)
        s1 = open(o + ".ulstate").read() if os.path.exists(o + ".ulstate") else None
        open(os.path.join(srv, "_opts"), "w").write("delay=0.05&etag=v1&ranges=0")
        kill_after(["-c", "8", "-o", o, url], out, 3.0)
        s2 = open(o + ".ulstate").read() if os.path.exists(o + ".ulstate") else None
        open(os.path.join(srv, "_opts"), "w").write("etag=v1")
        rc, txt, el, to = run(["-c", "8", "-o", o, url], out, 120)
        same = os.path.exists(o) and sha(o) == sha(os.path.join(srv, "a.bin"))
        report("stale .ulstate survives a non-range run, then resumes", not (rc == 0 and not same),
               "state unchanged by run2=%s; run3 rc=%s identical=%s first_diff=%s; %s" % (
                   s1 == s2 and s1 is not None, rc, same,
                   first_diff(o, os.path.join(srv, "a.bin")) if os.path.exists(o) else None, tail(txt, 2)))
    finally:
        S.stop()


def t_kill_resume_loop(n=40):
    d, srv, out = setup("killloop", {"a.bin": (40 * MiB + 12345, 11)})
    S = Server(srv)
    import random
    rnd = random.Random(42)
    try:
        o = os.path.join(out, "a.bin")
        url = S.url("/f/a.bin?delay=0.06&etag=x")
        kills = 0
        for i in range(n):
            rc = kill_after(["-c", str(rnd.choice([1, 2, 4, 8, 16])), "-o", o, url], out, rnd.uniform(0.3, 3.5))
            if rc is None:
                kills += 1
            if os.path.exists(o) and not os.path.exists(o + ".ulpart"):
                break
            # invariant: every byte the state claims is done must already be correct on disk
            st, part = o + ".ulstate", o + ".ulpart"
            if os.path.exists(st) and os.path.exists(part):
                segs = [tuple(map(int, l.split()[1:3])) for l in open(st) if l.startswith("seg ")]
                total = int([l for l in open(st) if l.startswith("total ")][0].split()[1])
                ref = open(os.path.join(srv, "a.bin"), "rb").read()
                got = open(part, "rb").read()
                missing = [False] * 0
                pos = 0
                bad = None
                for a, b in sorted(segs) + [(total, total)]:
                    if a > pos and got[pos:a] != ref[pos:a]:
                        k = next(j for j in range(pos, a) if j >= len(got) or got[j] != ref[j])
                        bad = k
                        break
                    pos = max(pos, b)
                if bad is not None:
                    report("kill -9 loop: state claims undownloaded bytes", False, "iteration %d offset %d" % (i, bad))
                    return
        rc, txt, el, to = run(["-c", "8", "-o", o, url], out, 120)
        same = os.path.exists(o) and sha(o) == sha(os.path.join(srv, "a.bin"))
        report("kill -9 loop (%d hard kills) then finish" % kills, same and rc == 0, "rc=%s identical=%s" % (rc, same))
    finally:
        S.stop()


def t_unique_name_resume():
    """a.bin already exists -> download goes to 'a (1).bin'; Ctrl+C/kill; rerun same command"""
    d, srv, out = setup("uniqresume", {"a.bin": (16 * MiB, 21)})
    S = Server(srv)
    try:
        make_file_fast(os.path.join(out, "a.bin"), 1000, 99)     # unrelated older file
        url = S.url("/f/a.bin?delay=0.15")
        kill_after(["-c", "4", url], out, 3.5)
        before = sorted(os.listdir(out))
        rc, txt, el, to = run(["-c", "4", url], out, 120)
        after = sorted(os.listdir(out))
        leftovers = [f for f in after if f.endswith((".ulpart", ".ulstate"))]
        report("resume of a download that was renamed 'name (1)'", not leftovers and "resuming" in txt,
               "before=%s after=%s resumed_msg=%s" % (before, after, "resuming" in txt))
    finally:
        S.stop()


def t_already_downloaded_false_positive():
    d, srv, out = setup("alreadydl", {"x.bin": (5000, 1)})
    S = Server(srv)
    try:
        make_file_fast(os.path.join(out, "x.bin"), 5000, 2)   # different content, same size
        rc, txt, el, to = run([S.url("/f/x.bin")], out, 30)
        same = sha(os.path.join(out, "x.bin")) == sha(os.path.join(srv, "x.bin"))
        files = sorted(os.listdir(out))
        report("same-name same-size different file => 'Already downloaded'", same or len(files) > 1,
               "rc=%s files=%s msg=%s" % (rc, files, tail(txt, 1)))
    finally:
        S.stop()


def t_empty_list():
    d = fresh_dir("emptylist")
    open(os.path.join(d, "list.txt"), "w").write("# nothing here\n\n")
    p = subprocess.Popen([EXE, "-d", d, "-i", os.path.join(d, "list.txt")], cwd=d, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    try:
        txt = p.communicate(input=b"", timeout=15)[0].decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        p.kill()
        txt = p.communicate()[0].decode("utf-8", "replace")
    report("-i with an empty list must not enter interactive mode", "Paste a link" not in txt,
           "rc=%s output: %s" % (p.returncode, tail(txt, 3)))


def t_redirect_loop():
    d = fresh_dir("redir")
    S = Server(d)
    try:
        rc, txt, el, to = run([S.url("/redirloop")], d, 60)
        report("redirect loop fails in bounded time", not to and rc == 1, "rc=%s %.1fs %s" % (rc, el, tail(txt, 1)))
    finally:
        S.stop()


def t_hang():
    d = fresh_dir("hang")
    S = Server(d)
    try:
        rc, txt, el, to = run(["-t", "5", "-R", "1", S.url("/hang")], d, 120)
        report("server accepts, never answers (-t 5 -R 1)", not to and rc == 1, "rc=%s %.1fs %s" % (rc, el, tail(txt, 1)))
    finally:
        S.stop()


def t_stall_ranged():
    """connections for ranges > 0 send headers and then nothing"""
    d, srv, out = setup("stall", {"a.bin": (8 * MiB, 4)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        rc, txt, el, to = run(["-t", "5", "-R", "1", "-c", "4", "-o", o, S.url("/f/a.bin?stall=ranged")], out, 150)
        same = os.path.exists(o) and sha(o) == sha(os.path.join(srv, "a.bin"))
        report("stalled range connections (-t 5 -R 1)", not to, "rc=%s %.1fs identical=%s %s" % (rc, el, same, tail(txt, 2)))
    finally:
        S.stop()


def t_drop_connections():
    d, srv, out = setup("drops", {"a.bin": (9 * MiB + 77, 8)})
    S = Server(srv)
    try:
        o = os.path.join(out, "a.bin")
        rc, txt, el, to = run(["-c", "6", "-o", o, S.url("/f/a.bin?dropafter=300000")], out, 180)
        same = os.path.exists(o) and sha(o) == sha(os.path.join(srv, "a.bin"))
        report("every response dropped after 300 KB", rc == 0 and same, "rc=%s %.1fs identical=%s %s" % (rc, el, same, tail(txt, 1)))
        o2 = os.path.join(out, "b.bin")
        rc, txt, el, to = run(["-c", "1", "-R", "3", "-o", o2, S.url("/f/a.bin?dropafter=300000&ranges=0")], out, 120)
        nreq = len([l for l in S.requests() if "ranges=0" in l])
        report("no-range server drops every response at 300 KB (-R 3)", not to,
               "rc=%s timed_out=%s %.0fs, %d requests" % (rc, to, el, nreq))
    finally:
        S.stop()


def t_content_disposition_names():
    names = {
        'attachment; filename="NUL .bin"': "NUL .bin",
        'attachment; filename="CONOUT$"': "CONOUT$",
        'attachment; filename="COM\xb9.txt"': None,
        'attachment; filename="..\\\\..\\\\evil.txt"': None,
        "attachment; filename*=UTF-8''..%2F..%2Fevil2.txt": None,
        'attachment; filename="a:b.txt"': None,
    }
    d, srv, out = setup("cdnames", {"p.bin": (5000, 3)})
    S = Server(srv)
    try:
        import urllib.parse
        for cd in names:
            sub = os.path.join(out, "d%d" % abs(hash(cd)))
            os.makedirs(sub)
            rc, txt, el, to = run(["-d", sub, S.url("/f/p.bin?cd=" + urllib.parse.quote(cd))], sub, 30)
            files = os.listdir(sub)
            escaped = [f for f in os.listdir(d) if "evil" in f] + [f for f in os.listdir(out) if "evil" in f]
            ok = rc == 0 and len(files) == 1 and not escaped
            report("Content-Disposition %r" % cd, ok, "rc=%s files=%s escaped=%s %s" % (rc, files, escaped, tail(txt, 2)))
    finally:
        S.stop()


TESTS = [t_sizes, t_second_conn_200, t_probe_lies_start, t_lying_content_length,
         lambda: t_change_mid_download(False), lambda: t_change_mid_download(True),
         lambda: t_resume_changed("noetag"), lambda: t_resume_changed("lastmod"), lambda: t_resume_changed("etag"),
         t_stale_state_after_200, t_kill_resume_loop, t_unique_name_resume, t_already_downloaded_false_positive,
         t_empty_list, t_redirect_loop, t_hang, t_stall_ranged, t_drop_connections, t_content_disposition_names]

if __name__ == "__main__":
    sel = sys.argv[1:]
    for t in TESTS:
        nm = getattr(t, "__name__", "")
        src = t.__code__.co_consts if nm == "<lambda>" else ()
        label = nm if nm != "<lambda>" else repr([c for c in t.__code__.co_names])
        if sel and not any(s in label or s in nm for s in sel):
            continue
        try:
            t()
        except Exception as e:  # keep going
            report(label, False, "harness exception: %r" % e)
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
