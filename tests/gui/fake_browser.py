"""Acts like a browser talking to the native host: puts itself into a kill-on-close
job object (no breakaway allowed), runs uloader.exe as the native messaging host and
sends one or more messages. Prints each JSON reply on its own line.

    python -I fake_browser.py HOST_EXE JSON [JSON ...]
"""
import ctypes
import ctypes.wintypes as W
import json
import struct
import subprocess
import sys

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
k32.CreateJobObjectW.restype = W.HANDLE
k32.GetCurrentProcess.restype = W.HANDLE
k32.SetInformationJobObject.argtypes = [W.HANDLE, ctypes.c_int, ctypes.c_void_p, W.DWORD]
k32.AssignProcessToJobObject.argtypes = [W.HANDLE, W.HANDLE]


class BASIC(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", W.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", W.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", W.DWORD), ("SchedulingClass", W.DWORD)]


class IOC(ctypes.Structure):
    _fields_ = [("a", ctypes.c_uint64 * 6)]


class EXT(ctypes.Structure):
    _fields_ = [("Basic", BASIC), ("Io", IOC), ("ProcessMemoryLimit", ctypes.c_size_t),
                ("JobMemoryLimit", ctypes.c_size_t), ("PeakProcessMemoryUsed", ctypes.c_size_t),
                ("PeakJobMemoryUsed", ctypes.c_size_t)]


job = k32.CreateJobObjectW(None, None)
info = EXT()
info.Basic.LimitFlags = 0x2000          # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, no BREAKAWAY_OK
k32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info))
if not k32.AssignProcessToJobObject(job, k32.GetCurrentProcess()):
    print("cannot join a job: %d" % ctypes.get_last_error())
    sys.exit(2)

host = subprocess.Popen([sys.argv[1], "chrome-extension://ednpijjpophhlcbidanppgjliblinjga/"],
                        stdin=subprocess.PIPE, stdout=subprocess.PIPE)
for msg in sys.argv[2:]:
    data = msg.encode("utf-8")
    host.stdin.write(struct.pack("<I", len(data)) + data)
    host.stdin.flush()
    n = struct.unpack("<I", host.stdout.read(4))[0]
    print(json.dumps(json.loads(host.stdout.read(n))), flush=True)
host.stdin.close()
host.wait(10)
# exiting closes the last job handle: everything still inside the job is killed
