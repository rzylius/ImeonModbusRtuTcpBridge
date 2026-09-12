#!/usr/bin/env python3
"""Continuously record IMEON operating transitions and suspected stuck charging."""

import argparse
import datetime as dt
import json
import signal
import sys
import time
from pathlib import Path

from live_write_test import Client


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat()


def append_jsonl(path, event):
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as stream:
        stream.write(json.dumps(event, separators=(",", ":")) + "\n")


def atomic_status(path, status):
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(status, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


def collect(client):
    settings = client.read(0x1300, 18)
    battery = client.read(0x0300, 4)
    grid = client.read(0x0100, 31)
    output = client.read(0x0200, 22)
    faults = client.read(0x0503, 6)
    mode = client.read(0x1323, 1)[0]
    metrics = client.read(37100, 22)
    return {
        "mode": mode,
        "power_management": settings[6],
        "pv_charge_limit_a": settings[3] / 10,
        "grid_charge_limit_a": settings[4] / 10,
        "discharge_limit_a": settings[5] / 10,
        "battery_voltage_v": battery[0] / 10,
        "battery_charge_a": battery[1] / 10,
        "battery_discharge_a": battery[2] / 10,
        "battery_soc_pct": battery[3],
        "grid_total_power_w": signed32(grid[9], grid[10]),
        "output_total_power_w": (output[9] << 16) | output[10],
        "fault_words": faults,
        "read_count": (metrics[0] << 16) | metrics[1],
        "read_errors": metrics[2],
        "write_count": (metrics[5] << 16) | metrics[6],
        "write_errors": metrics[7],
        "dropped_writes": metrics[12],
        "last_write_result": metrics[14],
        "write_queue": metrics[20],
        "controller_reboots": metrics[21],
    }


def signed32(high, low):
    value = (high << 16) | low
    return value - 0x100000000 if value & 0x80000000 else value


def material_state(state):
    return {key: state[key] for key in (
        "mode", "power_management", "pv_charge_limit_a",
        "grid_charge_limit_a", "discharge_limit_a", "write_count",
        "write_errors", "dropped_writes", "controller_reboots",
    )}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="10.0.20.231")
    parser.add_argument("--interval", type=int, default=60)
    parser.add_argument("--summary-interval", type=int, default=3600)
    parser.add_argument("--stuck-seconds", type=int, default=600)
    parser.add_argument("--charge-threshold", type=float, default=1.0)
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--status", type=Path, required=True)
    args = parser.parse_args()

    client = Client(args.host)
    running = True
    previous = None
    last_summary = 0.0
    possible_ignored_disable_at = None
    suspected_since = None

    def stop(_signal, _frame):
        nonlocal running
        running = False

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)

    while running:
        started = time.monotonic()
        try:
            state = collect(client)
            now = time.time()
            event = {"time": utc_now(), "kind": "sample", "host": args.host, "state": state}
            current_material = material_state(state)
            previous_material = material_state(previous) if previous else None

            if previous and state["write_count"] != previous["write_count"]:
                # The current firmware does not expose the successful command's
                # address/value. An unchanged AC-charge state following a write
                # is only a possible ignored disable, not proof of one.
                if (state["power_management"] == previous["power_management"] and
                        state["power_management"] & 0x4000 and
                        state["battery_charge_a"] >= args.charge_threshold):
                    possible_ignored_disable_at = now
                append_jsonl(args.log, {
                    "time": event["time"], "kind": "write_counter_change",
                    "from": previous["write_count"], "to": state["write_count"],
                    "before": previous_material, "after": current_material,
                })

            if previous_material != current_material:
                append_jsonl(args.log, {
                    "time": event["time"], "kind": "state_transition",
                    "before": previous_material, "after": current_material,
                })

            ac_charge_flag = bool(state["power_management"] & 0x4000)
            actively_charging = state["battery_charge_a"] >= args.charge_threshold
            if not ac_charge_flag or not actively_charging:
                possible_ignored_disable_at = None
            unchanged_after_write = (possible_ignored_disable_at is not None and
                                     now - possible_ignored_disable_at >= args.stuck_seconds)
            suspected = ac_charge_flag and actively_charging and unchanged_after_write
            if suspected and suspected_since is None:
                suspected_since = now
                append_jsonl(args.log, {
                    "time": event["time"], "kind": "suspected_stuck_grid_charge",
                    "reason": "AC-charge flag and measured charging persisted after a write",
                    "state": state,
                    "automatic_reboot": False,
                })
            elif not suspected:
                suspected_since = None

            if now - last_summary >= args.summary_interval:
                event["kind"] = "hourly_summary"
                append_jsonl(args.log, event)
                last_summary = now

            status = {
                "updated": event["time"], "healthy": not any(state["fault_words"]),
                "suspected_stuck_grid_charge": suspected,
                "suspected_since": dt.datetime.fromtimestamp(suspected_since, dt.timezone.utc).isoformat() if suspected_since else None,
                "note": "Alert is inferential until firmware exposes the last successful write address/value.",
                "state": state,
            }
            atomic_status(args.status, status)
            previous = state
        except Exception as error:
            append_jsonl(args.log, {
                "time": utc_now(), "kind": "communication_error",
                "error": f"{type(error).__name__}: {error}",
            })
            atomic_status(args.status, {
                "updated": utc_now(), "healthy": False,
                "communication_error": f"{type(error).__name__}: {error}",
            })

        elapsed = time.monotonic() - started
        time.sleep(max(1, args.interval - elapsed))

    append_jsonl(args.log, {"time": utc_now(), "kind": "monitor_stopped"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
