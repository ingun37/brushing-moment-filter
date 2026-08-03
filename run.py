#!/usr/bin/env python3
"""Convenience launcher: starts frame_server, then the DataGenUI client.

The server is stopped when the GUI exits (or on Ctrl-C). Extra pipeline
flags can be tweaked via the options below; defaults match dotnet/AGENTS.md.
"""

import argparse
import shutil
import signal
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
SERVER = ROOT / "cpp" / "build" / "frame_server"
CLIENT_DIR = ROOT / "dotnet" / "DataGenUI"
DOTNET_CANDIDATES = ["/usr/local/share/dotnet/dotnet", "dotnet"]


def find_dotnet() -> str:
    for candidate in DOTNET_CANDIDATES:
        if shutil.which(candidate):
            return candidate
    sys.exit("dotnet CLI not found (looked for " + ", ".join(DOTNET_CANDIDATES) + ")")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=15071)
    parser.add_argument("--sample-interval-seconds", type=float, default=1.0)
    parser.add_argument("--dedup-tolerance", type=float, default=5)
    args = parser.parse_args()

    if not SERVER.exists():
        sys.exit(f"{SERVER} not found — build it first:\n"
                 f"  cmake --build {ROOT / 'cpp' / 'build'} --target frame_server -j 8")

    server = subprocess.Popen([
        str(SERVER),
        "--port", str(args.port),
        "--sample-interval-seconds", str(args.sample_interval_seconds),
        "--dedup-tolerance", str(args.dedup_tolerance),
    ])
    print(f"frame_server running on port {args.port} (pid {server.pid})")

    try:
        if server.poll() is not None:
            return server.returncode  # e.g. port already in use
        return subprocess.run([find_dotnet(), "run", "--project", str(CLIENT_DIR)]).returncode
    except KeyboardInterrupt:
        return 0
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
            try:
                server.wait(timeout=5)
            except subprocess.TimeoutExpired:
                server.kill()
        print("frame_server stopped")


if __name__ == "__main__":
    sys.exit(main())
