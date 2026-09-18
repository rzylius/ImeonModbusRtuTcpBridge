#!/usr/bin/env python3
"""Build and run the firmware simulation without hardware or real secrets."""
import argparse
from pathlib import Path
import sys
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from prepare_modbusmaster import DEFAULT_SOURCE, prepare
from prepare_modbus_tcp import DEFAULT_SOURCE as TCP_SOURCE, prepare as prepare_tcp


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--modbusmaster", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--modbus-tcp", type=Path, default=TCP_SOURCE)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="imeon-host-tests-") as tmp:
        tmp = Path(tmp)
        for pattern in ("*.ino", "*.h"):
            for source in ROOT.glob(pattern):
                if source.name != "secrets.h":
                    shutil.copy2(source, tmp / source.name)
        shutil.copy2(ROOT / "secrets1.h", tmp / "secrets.h")
        binary = tmp / "firmware_test"
        subprocess.run([
            "g++", "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter",
            "-Wno-unused-variable", "-I", str(ROOT / "tests/fakes"),
            "-I", str(tmp), str(ROOT / "tests/firmware_test.cpp"), "-o", str(binary),
        ], check=True)
        for case in ("ordered_retry", "full_queue_retry", "exhausted_retry",
                     "freshness_age", "freshness_wrap", "freshness_rejected",
                     "freshness_failed_dropped", "sustained_writes",
                     "tcp_transaction_ids", "tcp_send_failures"):
            subprocess.run([str(binary), case], check=True)
        library = prepare(args.modbusmaster, tmp / "ModbusMaster")
        rtu_binary = tmp / "rtu_response_test"
        subprocess.run([
            "g++", "-std=c++17", "-I", str(ROOT / "tests/rtu_fakes"),
            "-I", str(library / "src"), str(ROOT / "tests/rtu_response_test.cpp"),
            str(library / "src/ModbusMaster.cpp"), "-o", str(rtu_binary),
        ], check=True)
        subprocess.run([str(rtu_binary)], check=True)
        tcp_library = prepare_tcp(args.modbus_tcp, tmp / "modbus-esp8266")
        transport = tmp / "transport"
        transport.mkdir()
        shutil.copy2(tcp_library / "src/ModbusTCPTemplate.h", transport)
        tcp_binary = tmp / "tcp_request_test"
        subprocess.run([
            "g++", "-std=c++17", "-I", str(transport),
            "-I", str(ROOT / "tests/tcp_fakes"),
            str(ROOT / "tests/tcp_request_test.cpp"), "-o", str(tcp_binary),
        ], check=True)
        subprocess.run([str(tcp_binary)], check=True)


if __name__ == "__main__":
    main()
