#!/usr/bin/env python3
"""Build and run the firmware simulation without hardware or real secrets."""
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="imeon-host-tests-") as tmp:
        tmp = Path(tmp)
        shutil.copy2(ROOT / "secrets1.h", tmp / "secrets.h")
        binary = tmp / "firmware_test"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-Wno-unused-variable", "-I", str(ROOT / "tests/fakes"),
            "-I", str(tmp), str(ROOT / "tests/firmware_test.cpp"), "-o", str(binary),
        ], check=True)
        for case in ("ordered_retry", "full_queue_retry", "exhausted_retry"):
            subprocess.run([str(binary), case], check=True)


if __name__ == "__main__":
    main()
