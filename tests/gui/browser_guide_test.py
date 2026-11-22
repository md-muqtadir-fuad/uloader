"""The "Add U-loader to your browser" window (#13).

  1. first start, the extension never connected: the window appears with the extension folder
  2. the extension says hello (the native host writes browser.txt): it shows "Connected" / "Done"
  3. after Done it is not shown again

The browser buttons are not clicked (they would open the real Edge / Chrome).
Run like the other window tests: a test build (ULOADER_TEST_CLASS) in tests/gui/build/,
with NO uloader.exe next to it (the window would register the browser connection).
"""
import os
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import uitest as U  # noqa: E402

sys.stdout.reconfigure(errors="replace")         # the status text has a check mark

HERE = os.path.dirname(os.path.abspath(__file__))
EXE = os.path.join(HERE, "build", "uloader-gui.exe")
T = os.path.join(HERE, "t4")
DATA, OUT = os.path.join(T, "data"), os.path.join(T, "out")
TITLE = "Add U-loader to your browser"
results = []


def ok(name, cond, detail=""):
    results.append(bool(cond))
    print(("PASS " if cond else "FAIL ") + name + ("  " + detail if detail else ""), flush=True)


def main():
    if os.path.exists(os.path.join(os.path.dirname(EXE), "uloader.exe")):
        print("refusing to run: uloader.exe next to the test build would register the browser connection")
        return 2
    shutil.rmtree(T, ignore_errors=True)
    os.makedirs(DATA)
    os.makedirs(OUT)
    with open(os.path.join(DATA, "settings.txt"), "w", encoding="utf-8") as f:
        f.write("dir=%s\nupdates=0\n" % OUT)        # browser_guide not set: a new user
    procs = []
    try:
        p = U.start_gui(EXE, DATA)
        procs.append(p)
        U.find_main()
        dlg = U.find_dialog(p.pid, TITLE, 10)
        ok("first start: the window appears", dlg)
        path = U.dlg_text(dlg, 1204) if dlg else ""
        ok("it shows the extension folder", path.lower().endswith("extension"), path)
        ok("it says the extension is not added yet", dlg and "Not added yet" in U.dlg_text(dlg, 1205))

        # the extension is added: its hello reaches the native host, which writes browser.txt
        with open(os.path.join(DATA, "browser.txt"), "w") as f:
            f.write("extension=connected\n")
        got = U.wait_for(lambda: "Connected" in U.dlg_text(dlg, 1205), 5, what="Connected")
        ok("it notices the extension", got and U.dlg_text(dlg, 2) == "Done", U.dlg_text(dlg, 1205))
        U.user32.PostMessageW(dlg, U.WM_COMMAND, 2, 0)            # Done
        U.wait_for(lambda: not U.user32.IsWindow(dlg), 5, what="window closed")
        main = U.find_main()
        U.command(main, "EXIT")
        p.wait(15)
        st = open(os.path.join(DATA, "settings.txt"), encoding="utf-8").read()
        ok("remembered as done", "browser_guide=0" in st)

        p = U.start_gui(EXE, DATA)
        procs.append(p)
        main = U.find_main()
        time.sleep(3)
        ok("not shown again", not [h for h in U.dialogs(p.pid) if U.title(h) == TITLE])
        U.command(main, "EXIT")
        p.wait(15)
    finally:
        for pr in procs:
            if pr.poll() is None:
                pr.kill()
    failed = results.count(False)
    print("\n%d/%d checks passed" % (len(results) - failed, len(results)))
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
