"""Run every U-loader adversarial suite against one binary.

python -I run_all.py <path\\to\\uloader.exe> [--v2]
  --v2 adds the suites that need the v2 code (redirect/cookie scoping, --job, native host, long paths).
Each suite prints PASS/FAIL lines; FAIL = defect reproduced (see the detail column).
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SUITES = ["t_direct.py", "t_hls.py", "t_ytdlp.py", "t_interactive.py", "t_leak.py"]
V2_SUITES = ["t_redirect.py", "t_job.py", "t_host.py", "t_longpath.py"]

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(2)
    env = dict(os.environ, ULOADER_EXE=os.path.abspath(sys.argv[1]), PYTHONIOENCODING="utf-8")
    suites = SUITES + (V2_SUITES if "--v2" in sys.argv else [])
    for s in suites:
        print("=== %s" % s, flush=True)
        try:
            subprocess.call([sys.executable, "-I", os.path.join(HERE, s)], cwd=HERE, env=env, timeout=1800)
        except subprocess.TimeoutExpired:
            print("SUITE TIMEOUT: %s" % s)
