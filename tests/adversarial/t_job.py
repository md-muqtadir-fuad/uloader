"""--job (job file from the browser bridge) tests.  python -I t_job.py   (ULOADER_EXE = v2 build)"""
import os, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa


def runjob(job, cwd, timeout=40):
    p = subprocess.Popen([EXE, "--job", job], cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    try:
        out = p.communicate(timeout=timeout)[0]
    except subprocess.TimeoutExpired:
        p.kill()
        out = p.communicate()[0]
    return p.returncode, out.decode("utf-8", "replace")


def main():
    d = fresh_dir("job")
    srv, out = os.path.join(d, "srv"), os.path.join(d, "out")
    os.makedirs(srv)
    os.makedirs(out)
    make_file_fast(os.path.join(srv, "f.bin"), 10000, 4)
    S = Server(srv)
    try:
        # 1. a non-job file passed to --job is deleted
        victim = os.path.join(d, "notes.txt")
        open(victim, "w").write("important")
        rc, txt = runjob(victim, d)
        report("--job with a file that is not a job file", os.path.exists(victim),
               "rc=%s file still exists=%s; %s" % (rc, os.path.exists(victim), tail(txt, 1)))
        # 2. hostile name + cookies file handling
        ck = os.path.join(d, "cookies.txt")
        open(ck, "w").write("# Netscape HTTP Cookie File\n127.0.0.1\tFALSE\t/\tFALSE\t0\tsid\tSECRET\n")
        job = os.path.join(d, "a.job")
        open(job, "w", newline="\n", encoding="utf-8").write("U-LOADER-JOB 1\nurl=%s\nmode=direct\nname=..\..\COM\u00b9.bin\ndir=%s\ncookies=%s\n"
                                           % (S.url("/f/f.bin"), out, ck))
        rc, txt = runjob(job, d)
        report("--job: name 'COM\u00b9.bin' (device) through the browser bridge", rc == 0 and len(os.listdir(out)) == 1,
               "rc=%s files=%s cookie file deleted=%s; %s" % (rc, os.listdir(out), not os.path.exists(ck), tail(txt, 2)))
    finally:
        S.stop()


if __name__ == "__main__":
    main()
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
