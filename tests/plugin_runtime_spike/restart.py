"""Each phase is a fresh native process with its own embedded interpreter."""
import subprocess
import sys
import tempfile

with tempfile.TemporaryDirectory(prefix="orca-process-restart-") as directory:
    for phase in ("install", "restart_enabled", "disable", "restart_disabled"):
        subprocess.run([sys.argv[1], sys.argv[2], phase, directory], check=True)
