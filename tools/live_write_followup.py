#!/usr/bin/env python3
"""Complete the isolated power-management tests after DHCP address migration."""

import argparse
from pathlib import Path

from live_write_test import Client, TestRun


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="10.0.20.231")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    run = TestRun(Client(args.host), args.output)
    baseline = run.snapshot("followup_baseline")

    # The prior 22-second observation did not show this bit clearing. Give the
    # inverter a full minute and several complete bridge polling rounds.
    run.command("grid_charge:disable_long_observation", 0x1306, [0xBFFF], settle=60)

    # Feed-in is an important ordinary energy-management flag. Preserve AC
    # output and avoid the separate grid-relay command.
    run.command("feed_in:disable", 0x1306, [0xDFFF], settle=25)
    run.command("feed_in:enable", 0x1306, [0x2000], settle=25)

    # Explicit idle attempt: disable every charge and discharge path. A bit is
    # considered applied only when delayed 0x1306 readback reflects it.
    disables = (
        ("pv_charge", 0x7FFF),
        ("grid_charge", 0xBFFF),
        ("load_with_pv", 0xEFFF),
        ("load_without_pv", 0xF7FF),
        ("grid_with_pv", 0xFBFF),
        ("grid_without_pv", 0xFDFF),
    )
    for name, value in disables:
        run.command("idle:disable_" + name, 0x1306, [value], settle=20)

    idle = run.snapshot("idle_observation")

    # Reconstruct exact baseline battery-flow/feed-in flags one bit per request.
    baseline_pm = int(baseline["power_management"], 16)
    operations = (
        (15, 0x8000, 0x7FFF),
        (14, 0x4000, 0xBFFF),
        (13, 0x2000, 0xDFFF),
        (12, 0x1000, 0xEFFF),
        (11, 0x0800, 0xF7FF),
        (10, 0x0400, 0xFBFF),
        (9, 0x0200, 0xFDFF),
    )
    for bit, enable, disable in operations:
        value = enable if baseline_pm & (1 << bit) else disable
        run.command(f"restore:bit_{bit}", 0x1306, [value], settle=18)

    final = run.snapshot("followup_final")
    run.record("summary", "followup_completed", baseline=baseline, idle=idle, final=final)


if __name__ == "__main__":
    main()
