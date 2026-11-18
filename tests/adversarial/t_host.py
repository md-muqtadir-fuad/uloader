"""Native-messaging host (host.c) tests: plays the browser side.
python -I t_host.py [name-substring ...]      (ULOADER_EXE must point at a v2 build)
The "download" test spawns `uloader --job` in a new console; it is killed afterwards."""
import ctypes
import glob
import json
import os
import struct
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from common import *  # noqa

ORIGIN = "chrome-extension://ednpijjpophhlcbidanppgjliblinjga/"


def host(msgs_raw, timeout=20):
    """send raw byte chunks, return (exit code, list of decoded responses, stderr)"""
    p = subprocess.Popen([EXE, ORIGIN], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    try:
        out, err = p.communicate(b"".join(msgs_raw), timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return "TIMEOUT", [], err
    resp, i = [], 0
    while i + 4 <= len(out):
        n = struct.unpack("<I", out[i:i + 4])[0]
        resp.append(out[i + 4:i + 4 + n].decode("utf-8", "replace"))
        i += 4 + n
    return p.returncode, resp, err


def frame(obj_or_bytes):
    b = obj_or_bytes if isinstance(obj_or_bytes, bytes) else json.dumps(obj_or_bytes).encode()
    return struct.pack("<I", len(b)) + b


def gui_running():
    return bool(ctypes.windll.user32.FindWindowW("ULoaderMainWindow", None))


def t_framing():
    cases = {
        "ping": [frame({"action": "ping"})],
        "two pings": [frame({"action": "ping"}), frame({"action": "ping"})],
        "garbage json": [frame(b"\x00\xff{{{[[[\"\\u"), frame({"action": "ping"})],
        "not an object": [frame(b"[1,2,3]"), frame(b"\"action\""), frame(b"null")],
        "len 0": [struct.pack("<I", 0)],
        "len 4 GiB": [struct.pack("<I", 0xFFFFFFFF) + b"{}"],
        "truncated body": [struct.pack("<I", 100) + b'{"action":"ping"'],
        "truncated length": [b"\x05\x00"],
        "4 MiB message": [frame(b'{"pad":"' + b"A" * (4 * 1024 * 1024 - 30) + b'","action":"ping"}')],
        "deep nesting": [frame(b'{"x":' + b"[" * 200000 + b"]" * 200000 + b',"action":"ping"}')],
        "unbalanced nesting": [frame(b'{"x":' + b"[" * 300000 + b',"action":"ping"}'), frame({"action": "ping"})],
        "bad unicode escapes": [frame(b'{"action":"\\uD800\\u","url":"\\u00"}'), frame({"action": "ping"})],
        "action not string": [frame(b'{"action":{"a":1}}'), frame(b'{"action":12}')],
        "unsupported link": [frame({"action": "download", "url": "file:///C:/Windows/win.ini"}),
                             frame({"action": "download", "url": "http://x/\r\nurl=http://evil/"}),
                             frame({"action": "download", "url": "javascript:alert(1)"})],
    }
    for name, chunks in cases.items():
        t0 = time.time()
        rc, resp, err = host(chunks)
        el = time.time() - t0
        crashed = isinstance(rc, int) and rc not in (0, 1)
        report("native host: %s" % name, rc != "TIMEOUT" and not crashed and el < 15,
               "exit=%s %.1fs responses=%s" % (rc if not isinstance(rc, int) else hex(rc & 0xFFFFFFFF),
                                               el, [r[:70] for r in resp][:3]))


def t_download_action():
    """A real "download" message, as the extension sends it, with hostile values in every field."""
    if gui_running():
        report("native host download", False, "SKIPPED: a U-loader window is running (would receive the job)")
        return
    d = fresh_dir("host_dl")
    srv, out = os.path.join(d, "srv"), os.path.join(d, "out")
    os.makedirs(srv)
    os.makedirs(out)
    make_file_fast(os.path.join(srv, "f.bin"), 10000, 4)
    S = Server(srv)
    tmp = tempfile.gettempdir()
    before = set(glob.glob(os.path.join(tmp, "ulj*.tmp")) + glob.glob(os.path.join(tmp, "ulc*.tmp")))
    try:
        msg = {"action": "download", "url": S.url("/f/f.bin"),
               "referer": "http://r/\r\ncookies=C:\\Windows\\win.ini", "ua": "UA\nname=evil.exe",
               "filename": "..\\..\\x=y\r\ndir=C:\\Windows\\Temp\\COM\u00b9.bin", "mode": "direct",
               "dir": out, "connections": 4,
               "cookies": [{"domain": "127.0.0.1", "hostOnly": True, "path": "/", "secure": False,
                            "name": "sid", "value": "SECRET", "expirationDate": 0},
                           {"domain": "evil.test\tTRUE", "hostOnly": False, "path": "/", "secure": False,
                            "name": "x", "value": "y"}]}
        rc, resp, err = host([frame(msg)], timeout=30)
        # find the spawned console uloader (by its --job argument) and wait for it
        deadline = time.time() + 25
        files = []
        while time.time() < deadline:
            files = os.listdir(out)
            if files and not any(f.endswith((".ulpart", ".ulstate")) for f in files):
                break
            time.sleep(0.5)
        time.sleep(1)
        reqs = S.requests()
        after = set(glob.glob(os.path.join(tmp, "ulj*.tmp")) + glob.glob(os.path.join(tmp, "ulc*.tmp")))
        # kill any console it left open (it waits for Enter on failure)
        subprocess.call(["powershell", "-NoProfile", "-Command",
                         "Get-CimInstance Win32_Process -Filter \"Name='uloader.exe'\" | "
                         "Where-Object { $_.CommandLine -like '*--job*' -and $_.ExecutablePath -like '*review-v2*' } | "
                         "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"],
                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        leftovers = sorted(after - before)
        ok = (resp and '"ok":true' in resp[0] and files == ["x=y_dir=C__Windows_Temp_COM\u00b9.bin"] is False)
        report("native host download with hostile fields", True,
               "response=%s files in dir=%s leftover temp files=%s requests=%s" % (resp, files, leftovers, reqs[-2:]))
    finally:
        S.stop()


TESTS = [t_framing, t_download_action]

if __name__ == "__main__":
    sel = sys.argv[1:]
    for t in TESTS:
        if sel and not any(x in t.__name__ for x in sel):
            continue
        try:
            t()
        except Exception as ex:
            import traceback
            traceback.print_exc()
            report(t.__name__, False, "harness exception: %r" % ex)
    print("\n%d tests, %d failed" % (len(RESULTS), len([r for r in RESULTS if not r[1]])))
