"""Resource-leak test: a 200-entry -i queue; samples handle count / private bytes / threads.
python -I t_leak.py [count]"""
import ctypes
import ctypes.wintypes as wt
import os
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

k32 = ctypes.windll.kernel32


def stats(pid):
    class PMC(ctypes.Structure):
        _fields_ = [("cb", wt.DWORD), ("PageFaultCount", wt.DWORD)] + \
                   [(n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                                   "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                                   "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage")]
    h = k32.OpenProcess(0x1000 | 0x0400 | 0x0010, False, pid)
    if not h:
        return None
    hc = wt.DWORD()
    k32.GetProcessHandleCount(h, ctypes.byref(hc))
    c = PMC()
    c.cb = ctypes.sizeof(c)
    ctypes.windll.psapi.GetProcessMemoryInfo(h, ctypes.byref(c), c.cb)
    k32.CloseHandle(h)
    # thread count via toolhelp
    TH32CS_SNAPTHREAD = 4

    class TE(ctypes.Structure):
        _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                    ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                    ("dwFlags", wt.DWORD)]
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = TE()
    te.dwSize = ctypes.sizeof(te)
    n = 0
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            n += 1
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return hc.value, c.PagefileUsage // 1024, n


def main():
    count = int(sys.argv[1]) if len(sys.argv) > 1 else 200
    d = fresh_dir("leak")
    srv, out = os.path.join(d, "srv"), os.path.join(d, "out")
    os.makedirs(os.path.join(srv, "h"))
    os.makedirs(out)
    make_file_fast(os.path.join(srv, "small.bin"), 20000, 1)
    make_file_fast(os.path.join(srv, "multi.bin"), 3 * (1 << 20), 2)
    for i in range(3):
        make_file_fast(os.path.join(srv, "h", "s%d.ts" % i), 5000, i)
    open(os.path.join(srv, "h", "p.m3u8"), "w", newline="\n").write(
        "#EXTM3U\n" + "".join("#EXTINF:1,\ns%d.ts\n" % i for i in range(3)) + "#EXT-X-ENDLIST\n")
    S = Server(srv)
    kinds = ["/f/small.bin?x=%d", "/f/multi.bin?x=%d", "/f/missing%d.bin", "/f/h/p.m3u8?x=%d", "/f/small.bin?x=%d&dropafter=5000"]
    with open(os.path.join(d, "list.txt"), "w") as f:
        for i in range(count):
            f.write(S.url(kinds[i % len(kinds)] % i) + "\n")
    p = subprocess.Popen([EXE, "-R", "1", "--overwrite", "-d", out, "-i", os.path.join(d, "list.txt")], cwd=out,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    import threading
    buf = []
    threading.Thread(target=lambda: buf.append(p.stdout.read()), daemon=True).start()
    samples = []
    t0 = time.time()
    while p.poll() is None and time.time() - t0 < 600:
        st = stats(p.pid)
        if st:
            samples.append((round(time.time() - t0, 1),) + st)
        time.sleep(0.5)
    if p.poll() is None:
        p.kill()
    p.wait()
    S.stop()
    txt = buf[0].decode("utf-8", "replace") if buf else ""
    if len(samples) < 4:
        report("leak", False, "too few samples %s" % samples)
        return
    q = len(samples) // 4
    early = samples[q]
    late = samples[-2]
    print("samples (t, handles, private KB, threads):", samples[::max(1, len(samples) // 12)])
    ok = late[1] - early[1] < 50 and late[2] - early[2] < 20000
    report("%d-URL queue: handles/memory stable" % count, ok,
           "handles %d -> %d, private KB %d -> %d, threads %d -> %d; exit=%s; %s" % (
               early[1], late[1], early[2], late[2], early[3], late[3], p.returncode, tail(txt, 1)))


if __name__ == "__main__":
    main()
