#!/usr/bin/env python3
"""Copy ModbusMaster 2.0.1 and apply the bridge patches without changing the source."""
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
DEFAULT_SOURCE = Path.home() / "Arduino/libraries/ModbusMaster"


def prepare(source, destination):
    source, destination = Path(source), Path(destination)
    if "version=2.0.1" not in (source / "library.properties").read_text():
        raise ValueError("The checked-in patches require ModbusMaster 2.0.1")
    shutil.copytree(source, destination)
    header = (destination / "src/ModbusMaster.h").read_text()
    patches = []
    if "ku16MBResponseTimeout          = 10000" not in header:
        patches.append("ModbusMaster-2.0.1-imeon-timeout.patch")
    if "#define MODBUSMASTER_STRICT_READ_LENGTH 1" not in header:
        patches.append("ModbusMaster-2.0.1-strict-read-length.patch")
    for name in patches:
        subprocess.run(["patch", "--batch", "--forward", "-p1", "-i",
                        str(ROOT / "patches" / name)], cwd=destination, check=True)
    return destination
