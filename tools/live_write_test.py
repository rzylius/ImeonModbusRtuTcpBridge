#!/usr/bin/env python3
"""Controlled live IMEON write test with before/after state recording."""

import argparse
import datetime as dt
import json
import socket
import struct
import time
from pathlib import Path


RANGES = (
    (0x0100, 31, "grid"),
    (0x0200, 22, "ac_output_and_distribution"),
    (0x0300, 4, "battery"),
    (0x0400, 16, "pv"),
    (0x0503, 6, "faults"),
    (0x1100, 2, "grid_injection_limit"),
    (0x1300, 18, "settings_1300"),
    (0x1323, 1, "mode"),
    (0x1405, 8, "threshold_groups"),
    (37100, 22, "bridge_metrics"),
)


class Client:
    def __init__(self, host):
        self.host = host
        self.tid = 1000

    def exchange(self, pdu):
        self.tid = (self.tid + 1) & 0xFFFF
        request = struct.pack(">HHHB", self.tid, 0, len(pdu) + 1, 1) + pdu
        with socket.create_connection((self.host, 502), 4) as sock:
            sock.settimeout(8)
            sock.sendall(request)
            header = self._recv(sock, 7)
            transaction, protocol, length, unit = struct.unpack(">HHHB", header)
            if transaction != self.tid or protocol != 0 or unit != 1:
                raise RuntimeError(f"invalid MBAP header: {header.hex()}")
            return self._recv(sock, length - 1)

    @staticmethod
    def _recv(sock, size):
        data = b""
        while len(data) < size:
            part = sock.recv(size - len(data))
            if not part:
                raise RuntimeError("connection closed during response")
            data += part
        return data

    def read(self, address, count):
        response = self.exchange(struct.pack(">BHH", 3, address, count))
        if response[0] != 3:
            raise RuntimeError(f"read 0x{address:04X} failed: {response.hex()}")
        if response[1] != count * 2:
            raise RuntimeError(f"wrong read length at 0x{address:04X}: {response.hex()}")
        return list(struct.unpack(">" + "H" * count, response[2:]))

    def write(self, address, values):
        body = struct.pack(">HHB", address, len(values), len(values) * 2)
        body += struct.pack(">" + "H" * len(values), *values)
        response = self.exchange(bytes([16]) + body)
        expected = struct.pack(">BHH", 16, address, len(values))
        if response != expected:
            raise RuntimeError(f"write 0x{address:04X} rejected: {response.hex()}")
        return response.hex()


def signed32(high, low):
    value = (high << 16) | low
    return value - 0x100000000 if value & 0x80000000 else value


def decode(raw):
    grid = raw["grid"]
    battery = raw["battery"]
    settings = raw["settings_1300"]
    metrics = raw["bridge_metrics"]
    return {
        "grid_voltage_v": grid[0] / 10,
        "grid_frequency_hz": grid[23] / 10,
        "grid_power_words_signed": [signed32(grid[i], grid[i + 1]) for i in (3, 5, 7, 9)],
        "power_factor": grid[30] / 100,
        "battery_voltage_v": battery[0] / 10,
        "battery_charge_a": battery[1] / 10,
        "battery_discharge_a": battery[2] / 10,
        "battery_soc_pct": battery[3],
        "fault_words": raw["faults"],
        "grid_injection_limit_w": (raw["grid_injection_limit"][0] << 16) | raw["grid_injection_limit"][1],
        "pv_charge_limit_a": settings[3] / 10,
        "grid_charge_limit_a": settings[4] / 10,
        "discharge_limit_a": settings[5] / 10,
        "power_management": f"0x{settings[6]:04X}",
        "mode": raw["mode"][0],
        "threshold_groups": raw["threshold_groups"],
        "read_errors": metrics[2],
        "write_count": (metrics[5] << 16) | metrics[6],
        "write_errors": metrics[7],
        "dropped_writes": metrics[12],
        "last_failed_address": f"0x{metrics[13]:04X}",
        "last_write_result": metrics[14],
        "write_queue": metrics[20],
        "reboots": metrics[21],
    }


class TestRun:
    def __init__(self, client, output):
        self.client = client
        self.output = output
        self.events = []

    def record(self, kind, name, **fields):
        event = {"time": dt.datetime.now(dt.timezone.utc).isoformat(), "kind": kind, "name": name, **fields}
        self.events.append(event)
        self.output.write_text(json.dumps(self.events, indent=2) + "\n")
        print(json.dumps(event), flush=True)

    def snapshot(self, name):
        raw = {}
        for address, count, label in RANGES:
            raw[label] = self.client.read(address, count)
            time.sleep(0.08)
        state = decode(raw)
        self.record("snapshot", name, state=state, raw=raw)
        if any(raw["faults"]):
            raise RuntimeError(f"fault/warning appeared at {name}: {raw['faults']}")
        if state["write_errors"] or state["dropped_writes"] or state["last_write_result"]:
            raise RuntimeError(f"bridge write diagnostic failure at {name}: {state}")
        return state

    def command(self, name, address, values, settle=16):
        before = self.snapshot(name + ":before")
        response = self.client.write(address, values)
        self.record("command", name, address=f"0x{address:04X}", values=values, tcp_response=response)
        time.sleep(3)
        immediate = self.snapshot(name + ":acknowledged")
        time.sleep(max(0, settle - 3))
        settled = self.snapshot(name + ":settled")
        if settled["write_count"] <= before["write_count"]:
            raise RuntimeError(f"RTU write count did not advance for {name}")
        return before, immediate, settled


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="10.0.20.52")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run = TestRun(Client(args.host), args.output)
    baseline = run.snapshot("baseline")
    original_limits = [int(baseline["pv_charge_limit_a"] * 10), int(baseline["grid_charge_limit_a"] * 10), int(baseline["discharge_limit_a"] * 10)]
    original_mode = baseline["mode"]

    # Conservative one-amp changes, each followed by restoration.
    for name, address, original in zip(("pv_charge_limit", "grid_charge_limit", "discharge_limit"), (0x1303, 0x1304, 0x1305), original_limits):
        alternate = original - 10
        run.command(name + ":change", address, [alternate])
        run.command(name + ":restore", address, [original])

    # Both relevant running modes. Mode changes are deliberately observed before
    # reapplying limits because IMEON may reset them as a side effect.
    run.command("mode:SmartGrid", 0x1323, [1], settle=35)
    run.command("mode:Backup", 0x1323, [2], settle=35)
    for name, address, original in zip(("pv", "grid", "discharge"), (0x1303, 0x1304, 0x1305), original_limits):
        current = run.snapshot("post_mode_limit_check:" + name)
        key = {"pv": "pv_charge_limit_a", "grid": "grid_charge_limit_a", "discharge": "discharge_limit_a"}[name]
        if int(current[key] * 10) != original:
            run.command("post_mode_limit_restore:" + name, address, [original])

    # Exercise every battery-flow bit. A disable after each enable returns that
    # function to idle and makes the effect independently attributable.
    flow_tests = (
        ("charge_from_pv", 0x8000, 0x7FFF),
        ("charge_from_grid", 0x4000, 0xBFFF),
        ("discharge_to_loads_with_pv", 0x1000, 0xEFFF),
        ("discharge_to_loads_without_pv", 0x0800, 0xF7FF),
        ("discharge_to_grid_with_pv", 0x0400, 0xFBFF),
        ("discharge_to_grid_without_pv", 0x0200, 0xFDFF),
    )
    for name, enable, disable in flow_tests:
        run.command(name + ":enable", 0x1306, [enable], settle=22)
        run.command(name + ":disable", 0x1306, [disable], settle=22)

    # Explicit battery-idle state: all charge/discharge paths disabled. Preserve
    # feed-in and AC-output bits, then reconstruct the exact original bit field.
    for name, disable in (("pv", 0x7FFF), ("grid", 0xBFFF), ("load_pv", 0xEFFF), ("load_no_pv", 0xF7FF), ("export_pv", 0xFBFF), ("export_no_pv", 0xFDFF)):
        run.command("battery_idle:disable_" + name, 0x1306, [disable], settle=12)

    # Restore original power-management flags one bit at a time.
    original_pm = int(baseline["power_management"], 16)
    for bit, enable, disable in ((15,0x8000,0x7FFF),(14,0x4000,0xBFFF),(13,0x2000,0xDFFF),(12,0x1000,0xEFFF),(11,0x0800,0xF7FF),(10,0x0400,0xFBFF),(9,0x0200,0xFDFF)):
        run.command(f"restore_power_management_bit_{bit}", 0x1306, [enable if original_pm & (1 << bit) else disable], settle=12)

    if original_mode in (1, 2) and run.snapshot("pre_final_mode_restore")["mode"] != original_mode:
        run.command("restore_original_mode", 0x1323, [original_mode], settle=35)
    final = run.snapshot("final")
    run.record("summary", "completed", baseline=baseline, final=final)


if __name__ == "__main__":
    main()
