#!/usr/bin/env python3
"""Copy modbus-esp8266 4.1.0 and fix its minimum TCP frame-size check."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = Path.home() / "Arduino/libraries/modbus-esp8266"


def prepare(source, destination):
    source, destination = Path(source), Path(destination)
    if "version=4.1.0" not in (source / "library.properties").read_text():
        raise ValueError("The checked-in TCP patch requires modbus-esp8266 4.1.0")
    shutil.copytree(source, destination)
    header = (destination / "src/ModbusTCPTemplate.h").read_bytes()
    if b"#define MODBUSIP_COMPLETE_MINIMAL_FRAMES 1" not in header:
        subprocess.run(["patch", "--batch", "--forward", "-p1", "-i",
                        str(ROOT / "patches/modbus-esp8266-4.1.0-minimal-frame.patch")],
                       cwd=destination, check=True)
    return destination
