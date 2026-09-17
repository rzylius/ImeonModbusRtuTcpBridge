#!/usr/bin/env python3
"""Compile an isolated ESP32-S3 sketch with public placeholder configuration."""
import argparse
from pathlib import Path
import shutil
import subprocess
import tempfile
from prepare_modbusmaster import DEFAULT_SOURCE, prepare

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, action="append", default=[])
    parser.add_argument("--modbusmaster", type=Path, default=DEFAULT_SOURCE)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="imeon-build-") as tmp:
        sketch = Path(tmp) / "imeon_bridge_104_rw"
        sketch.mkdir()
        for source in ROOT.glob("*.ino"):
            shutil.copy2(source, sketch / source.name)
        for source in ROOT.glob("*.h"):
            if source.name != "secrets.h":
                shutil.copy2(source, sketch / source.name)
        shutil.copy2(ROOT / "secrets1.h", sketch / "secrets.h")
        command = ["arduino-cli", "compile", "--fqbn", "esp32:esp32:esp32s3",
                   "--board-options", "LoopCore=0,EventsCore=0",
                   "--build-path", str(Path(tmp) / "build")]
        patched = prepare(args.modbusmaster, Path(tmp) / "ModbusMaster")
        command += ["--library", str(patched)]
        for library in args.library:
            command += ["--library", str(library.resolve())]
        subprocess.run(command + [str(sketch)], check=True)


if __name__ == "__main__":
    main()
