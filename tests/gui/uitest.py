"""Helpers to drive and inspect the U-loader window from Python (ctypes)."""
import ctypes
import ctypes.wintypes as W
import hashlib
import os
import socket
import subprocess
import time

from PIL import Image

user32 = ctypes.WinDLL("user32", use_last_error=True)
gdi32 = ctypes.WinDLL("gdi32", use_last_error=True)
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)

try:
    user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))
except Exception:
    pass

LPARAM = ctypes.c_ssize_t
WPARAM = ctypes.c_size_t
user32.FindWindowW.argtypes = [W.LPCWSTR, W.LPCWSTR]
user32.FindWindowW.restype = W.HWND
user32.FindWindowExW.argtypes = [W.HWND, W.HWND, W.LPCWSTR, W.LPCWSTR]
user32.FindWindowExW.restype = W.HWND
user32.SendMessageW.argtypes = [W.HWND, W.UINT, WPARAM, LPARAM]
user32.SendMessageW.restype = LPARAM
user32.PostMessageW.argtypes = [W.HWND, W.UINT, WPARAM, LPARAM]
user32.PostMessageW.restype = W.BOOL
user32.SendMessageTimeoutW.argtypes = [W.HWND, W.UINT, WPARAM, LPARAM, W.UINT, W.UINT, ctypes.POINTER(ctypes.c_size_t)]
user32.SendMessageTimeoutW.restype = LPARAM
user32.GetWindowRect.argtypes = [W.HWND, ctypes.POINTER(W.RECT)]
user32.GetClientRect.argtypes = [W.HWND, ctypes.POINTER(W.RECT)]
user32.PrintWindow.argtypes = [W.HWND, W.HDC, W.UINT]
user32.GetDC.argtypes = [W.HWND]
user32.GetDC.restype = W.HDC
user32.ReleaseDC.argtypes = [W.HWND, W.HDC]
user32.IsWindowVisible.argtypes = [W.HWND]
user32.IsWindow.argtypes = [W.HWND]
user32.GetWindowThreadProcessId.argtypes = [W.HWND, ctypes.POINTER(W.DWORD)]
user32.GetClassNameW.argtypes = [W.HWND, W.LPWSTR, ctypes.c_int]
user32.GetWindowTextW.argtypes = [W.HWND, W.LPWSTR, ctypes.c_int]
user32.GetDlgItem.argtypes = [W.HWND, ctypes.c_int]
user32.GetDlgItem.restype = W.HWND
user32.OpenClipboard.argtypes = [W.HWND]
user32.SetClipboardData.argtypes = [W.UINT, W.HANDLE]
user32.SetClipboardData.restype = W.HANDLE
user32.GetClipboardData.argtypes = [W.UINT]
user32.GetClipboardData.restype = W.HANDLE
user32.SetWindowPos.argtypes = [W.HWND, W.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, W.UINT]
gdi32.CreateCompatibleDC.argtypes = [W.HDC]
gdi32.CreateCompatibleDC.restype = W.HDC
gdi32.CreateCompatibleBitmap.argtypes = [W.HDC, ctypes.c_int, ctypes.c_int]
gdi32.CreateCompatibleBitmap.restype = W.HBITMAP
gdi32.SelectObject.argtypes = [W.HDC, W.HGDIOBJ]
gdi32.SelectObject.restype = W.HGDIOBJ
gdi32.DeleteObject.argtypes = [W.HGDIOBJ]
gdi32.DeleteDC.argtypes = [W.HDC]
gdi32.GetDIBits.argtypes = [W.HDC, W.HBITMAP, W.UINT, W.UINT, ctypes.c_void_p, ctypes.c_void_p, W.UINT]
kernel32.GlobalAlloc.argtypes = [W.UINT, ctypes.c_size_t]
kernel32.GlobalAlloc.restype = W.HGLOBAL
kernel32.GlobalLock.argtypes = [W.HGLOBAL]
kernel32.GlobalLock.restype = ctypes.c_void_p
kernel32.GlobalUnlock.argtypes = [W.HGLOBAL]

CLASS = os.environ.get("ULOADER_TEST_CLASS", "ULoaderMainWindow")   # a test build may use its own
WM_NULL, WM_CLOSE, WM_KEYDOWN, WM_KEYUP, WM_COMMAND, WM_COPYDATA = 0x0, 0x10, 0x100, 0x101, 0x111, 0x4A
LVM_GETITEMCOUNT, LVM_GETSELECTEDCOUNT = 0x1004, 0x1032
SMTO_ABORTIFHUNG = 2
UL_COPYDATA_JOB = 0x554C4A42
IDM = dict(ADD=40001, RESUME=40002, PAUSE=40003, PAUSEALL=40004, DELETE=40005, DELETEFILE=40006, OPEN=40007,
           OPENFOLDER=40008, SETTINGS=40009, COPYLINK=40010, PASTE=40011, SELECTALL=40012, SHOW=40013,
           EXIT=40014, RESUMEALL=40015)
IDCANCEL = 2
VK_HOME, VK_END, VK_DOWN, VK_UP = 0x24, 0x23, 0x28, 0x26


class COPYDATASTRUCT(ctypes.Structure):
    _fields_ = [("dwData", ctypes.c_size_t), ("cbData", W.DWORD), ("lpData", ctypes.c_void_p)]


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


def wait_for(pred, timeout, step=0.2, what="condition"):
    t0 = time.time()
    while time.time() - t0 < timeout:
        v = pred()
        if v:
            return v
        time.sleep(step)
    raise TimeoutError("timed out after %.0fs waiting for %s" % (timeout, what))


def find_main(timeout=10):
    return wait_for(lambda: user32.FindWindowW(CLASS, None), timeout, what="main window")


def listview(main):
    return user32.FindWindowExW(main, None, "SysListView32", None)


def item_count(main):
    return user32.SendMessageW(listview(main), LVM_GETITEMCOUNT, 0, 0)


def selected_count(main):
    return user32.SendMessageW(listview(main), LVM_GETSELECTEDCOUNT, 0, 0)


def command(main, idm):
    user32.PostMessageW(main, WM_COMMAND, IDM[idm] if isinstance(idm, str) else idm, 0)


def key(hwnd, vk, times=1):
    for _ in range(times):
        user32.PostMessageW(hwnd, WM_KEYDOWN, vk, 1)
        user32.PostMessageW(hwnd, WM_KEYUP, vk, 0xC0000001)


def ping(main, timeout_ms=5000):
    """Round trip of a WM_NULL through the UI thread, in milliseconds (None = hung)."""
    res = ctypes.c_size_t()
    t0 = time.perf_counter()
    ok = user32.SendMessageTimeoutW(main, WM_NULL, 0, 0, SMTO_ABORTIFHUNG, timeout_ms, ctypes.byref(res))
    return (time.perf_counter() - t0) * 1000 if ok else None


def send_job(main, jobpath):
    data = ctypes.create_string_buffer(jobpath.encode("utf-8"))
    cds = COPYDATASTRUCT(UL_COPYDATA_JOB, len(data), ctypes.cast(data, ctypes.c_void_p))
    res = ctypes.c_size_t()
    ok = user32.SendMessageTimeoutW(main, WM_COPYDATA, 0, ctypes.addressof(cds), SMTO_ABORTIFHUNG, 8000,
                                    ctypes.byref(res))
    return bool(ok) and res.value == 1      # 1 = taken, 2 = refused, 0 = the window is closing


def write_job(path, **kv):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write("U-LOADER-JOB 1\n")
        for k, v in kv.items():
            f.write("%s=%s\n" % (k, v))
    return path


def window_pid(hwnd):
    pid = W.DWORD()
    user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
    return pid.value


def class_name(hwnd):
    b = ctypes.create_unicode_buffer(256)
    user32.GetClassNameW(hwnd, b, 256)
    return b.value


def title(hwnd):
    b = ctypes.create_unicode_buffer(512)
    user32.GetWindowTextW(hwnd, b, 512)
    return b.value


def dialogs(pid):
    out = []
    proto = ctypes.WINFUNCTYPE(W.BOOL, W.HWND, LPARAM)

    def cb(h, lp):
        if window_pid(h) == pid and class_name(h) == "#32770" and user32.IsWindowVisible(h):
            out.append(h)
        return True

    user32.EnumWindows(proto(cb), 0)
    return out


def find_dialog(pid, caption, timeout=10):
    return wait_for(lambda: next((h for h in dialogs(pid) if title(h) == caption), None), timeout,
                    what="dialog " + caption)


def dlg_text(dlg, ctl_id):
    h = user32.GetDlgItem(dlg, ctl_id)
    b = ctypes.create_unicode_buffer(65536)
    user32.SendMessageW(h, 0x000D, 65536, ctypes.addressof(b))   # WM_GETTEXT (marshalled)
    return b.value


def capture(hwnd, path):
    r = W.RECT()
    user32.GetWindowRect(hwnd, ctypes.byref(r))
    w, h = r.right - r.left, r.bottom - r.top
    sdc = user32.GetDC(None)
    mdc = gdi32.CreateCompatibleDC(sdc)
    bmp = gdi32.CreateCompatibleBitmap(sdc, w, h)
    old = gdi32.SelectObject(mdc, bmp)
    user32.PrintWindow(hwnd, mdc, 2)  # PW_RENDERFULLCONTENT
    gdi32.SelectObject(mdc, old)

    class BIH(ctypes.Structure):
        _fields_ = [("biSize", W.DWORD), ("biWidth", W.LONG), ("biHeight", W.LONG), ("biPlanes", W.WORD),
                    ("biBitCount", W.WORD), ("biCompression", W.DWORD), ("biSizeImage", W.DWORD),
                    ("biXPelsPerMeter", W.LONG), ("biYPelsPerMeter", W.LONG), ("biClrUsed", W.DWORD),
                    ("biClrImportant", W.DWORD)]

    bih = BIH(ctypes.sizeof(BIH), w, -h, 1, 32, 0, 0, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(w * h * 4)
    gdi32.GetDIBits(mdc, bmp, 0, h, buf, ctypes.byref(bih), 0)
    gdi32.DeleteObject(bmp)
    gdi32.DeleteDC(mdc)
    user32.ReleaseDC(None, sdc)
    img = Image.frombuffer("RGB", (w, h), buf.raw, "raw", "BGRX", 0, 1)
    img.save(path)
    return path


def set_clipboard(text):
    for _ in range(20):
        if user32.OpenClipboard(None):
            break
        time.sleep(0.05)
    else:
        raise RuntimeError("cannot open the clipboard")
    try:
        user32.EmptyClipboard()
        data = (text + "\0").encode("utf-16-le")
        h = kernel32.GlobalAlloc(0x0002, len(data))
        p = kernel32.GlobalLock(h)
        ctypes.memmove(p, data, len(data))
        kernel32.GlobalUnlock(h)
        user32.SetClipboardData(13, h)
    finally:
        user32.CloseClipboard()


def get_clipboard():
    if not user32.OpenClipboard(None):
        return None
    try:
        h = user32.GetClipboardData(13)
        if not h:
            return None
        p = kernel32.GlobalLock(h)
        s = ctypes.wstring_at(p)
        kernel32.GlobalUnlock(h)
        return s
    finally:
        user32.CloseClipboard()


def read_list(datadir):
    """Parse downloads.txt into a list of dicts."""
    path = os.path.join(datadir, "downloads.txt")
    if not os.path.exists(path):
        return []
    recs, cur = [], None
    with open(path, encoding="utf-8") as f:
        for line in f.read().splitlines():
            if line == "[download]":
                cur = {}
            elif line == "end":
                if cur is not None:
                    recs.append(cur)
                cur = None
            elif cur is not None and "=" in line:
                k, v = line.split("=", 1)
                cur[k] = v
    return recs


def start_gui(exe, datadir, *args, env_extra=None):
    env = dict(os.environ)
    env["ULOADER_DATA"] = datadir
    if env_extra:
        env.update(env_extra)
    return subprocess.Popen([exe] + list(args), env=env)
