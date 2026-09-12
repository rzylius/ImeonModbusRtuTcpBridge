# IMEON live write-operation test — 2026-08-23

## Scope and evidence

The live inverter was exercised through the ESP32-S3 Modbus RTU/TCP bridge.
Every operation was evaluated at three distinct levels:

1. Modbus TCP function `0x10` response from the bridge.
2. RTU acknowledgment reported by the bridge counters.
3. Delayed inverter register readback and measurement/fault telemetry.

Raw, timestamped snapshots are in `test-results/`. They include every telemetry
range cached by the bridge (`0x0100`, `0x0200`, `0x0300`, `0x0400`, `0x0503`,
`0x1000`, `0x1100`, `0x1300`, `0x1323`, `0x1405`, and bridge diagnostics).

The controller migrated by DHCP from `10.0.20.52` to `10.0.20.231` during the
test. This initially looked like a TCP-service failure; the ESP32 had continued
running and did not reboot. OpenHAB writes were subsequently disabled from its
menu. The final 56-second quiet window showed a stable write counter.

## Restored final state

| Item | Final value |
|---|---:|
| Running mode | Backup (`0x0002`) |
| Power management | `0x6100` |
| Maximum PV charge current | 100.0 A |
| Maximum grid charge current | 90.0 A |
| Maximum discharge current | 119.0 A |
| Grid injection limit | 9000 W |
| Battery thresholds | `450, 460, 450, 460, 10, 40, 20, 85` |
| Fault/warning words | all zero |
| Write errors / dropped / queued | `0 / 0 / 0` |

Nine final samples remained unchanged. Battery telemetry at the final snapshot
was 53.3 V, 2.1 A charge, 0 A discharge, and 100% SOC.

## Current-limit writes

All three current limits passed TCP response, RTU acknowledgment, delayed
readback, and restoration tests:

| Setting | Test transition | Result |
|---|---:|---|
| PV charge limit (`0x1303`) | 100.0 → 99.0 → 100.0 A | Pass |
| Grid charge limit (`0x1304`) | 2.5 → 1.5 → 2.5 A | Pass |
| Discharge limit (`0x1305`) | 124.0 → 123.0 → 124.0 A | Pass |

The final grid/discharge targets of 90.0/119.0 A were separately restored and
confirmed after mode testing and OpenHAB isolation.

## Backup and SmartGrid mode behavior

Both relevant modes applied successfully after their documented hardware delay:

| Operation | Mode readback | `0x1306` side effect | Numeric limits |
|---|---:|---:|---|
| Backup → SmartGrid | `1` | `0x6100` → `0xF900` | initially unchanged |
| SmartGrid → Backup | `2` | `0xF900` → `0xE100` | initially unchanged |

The mode operations rewrote several energy-management flags. Numeric current
limits appeared unchanged during the initial 35-second observation but later
alternated between the installation values and mode defaults while an external
writer was still active. After OpenHAB was disabled, values were explicitly
restored and remained stable. Mode changes must therefore always be followed by
delayed verification and reapplication of installation-specific limits.

## Power-management operations (`0x1306`)

### Flag readback

These bit operations applied and reversed correctly in delayed readback:

| Operation | Enabled state | Disabled/base state |
|---|---:|---:|
| Charge from PV | `0xE100` | `0x6100` |
| Discharge to loads with PV | `0x7100` | `0x6100` |
| Discharge to loads without PV | `0x6900` | `0x6100` |
| Discharge to grid with PV | `0x6500` | `0x6100` |
| Discharge to grid without PV | `0x6300` | `0x6100` |

The battery did not physically discharge during the discharge tests. Grid
charging remained enabled, battery SOC was 100%, and battery discharge telemetry
remained 0 A. These operations therefore pass configuration readback, but actual
battery-to-load/grid energy flow is not proven under the observed conditions.

### Charge from grid

The baseline `0x6100` already had bit 14 enabled, and battery charge telemetry
was approximately 2.1–2.8 A. Sending enable was an acknowledged no-op, as
expected. Sending the documented disable value `0xBFFF` was acknowledged by TCP
and RTU but did **not** clear bit 14, even after a full 60-second observation.
This is an inverter application-level failure, not a bridge transport failure.

### Feed-in and effective idle behavior

Feed-in control had a real and coupled effect:

- Disable (`0xDFFF`) changed `0x1306` from `0x6100` to `0x0100`.
- It cleared both feed-in and grid-charge flags, leaving only AC output enabled.
- Total grid power changed from approximately 1.86 kW export to 0.29 kW import.
- Enable (`0x2000`) restored `0x6100` and approximately 1.88 kW export.

Thus, on this inverter, the observed effective command for a battery-idle flag
configuration is feed-in disable, which also clears grid charging. Sequentially
sending the six individual charge/discharge disable commands while feed-in was
enabled did not clear grid charging and left `0x1306` at `0x6100`.

Battery charge telemetry still showed roughly 2.3 A at the first `0x0100`
snapshot, so a zero-current physical idle state was not conclusively observed in
the available settling window. The register state and reversal of grid flow were
confirmed.

## Transport and controller observations

- All isolated-run TCP write requests returned correct function-`0x10` echoes.
- The successful isolated/follow-up writes had zero RTU write errors and drops.
- One telemetry read error accumulated late in the run; polling recovered and
  continued normally.
- A pre-reboot attempt at `0x1303` encountered four RTU response timeouts and was
  dropped after retries without changing the inverter. After controller reboot
  and activation, the same operation succeeded and read back correctly.
- LED activity while `.52` was unreachable was consistent with the controller
  continuing to run. The actual cause was DHCP migration to `.231`, not a reboot.

## Operational conclusions

1. Never equate the immediate TCP echo or RTU acknowledgment with application.
   Always verify delayed `0x1306`/setting readback and actual power telemetry.
2. Treat Backup/SmartGrid changes as compound operations. Re-read all limits and
   flags after at least one complete telemetry cycle and restore intended values.
3. Grid-charge disable is not independently reliable in Backup on this unit.
4. Feed-in disable also clears grid charging and is the only tested operation
   that produced the `0x0100` idle flag state.
5. Energy-flow tests are condition-dependent. Repeat discharge tests when SOC,
   PV, load, and grid conditions permit actual discharge if physical-flow proof
   is required; the present run proves only command application/readback.
6. Isolate external automation before testing and verify isolation by watching
   the bridge write counter, not merely by changing the controller IP.

## Subsequent stuck-charge recovery observation

SmartGrid later remained at `0xF900` and charged the battery at about 2.1 A.
Two isolated `0x1306 = 0xBFFF` attempts received TCP and RTU acknowledgments but
were not applied after 60 seconds. The inverter was then rebooted while the
ESP32 remained online. Bridge reads stopped for approximately seven minutes;
cached `0xF900` values during that interval were stale.

When fresh telemetry resumed, the inverter reported `0x3900`, AC charge current
fell to zero, and battery discharge resumed. A requested 90.0 A maximum
discharge limit was acknowledged and confirmed at 30, 60, and 90 seconds. A
later sample measured 49.4 V and 89.6 A battery discharge, demonstrating that
the inverter was physically following the recovered discharge-to-load/export
state. The high current was close to the configured maximum and was not merely
a flag/readback result.

Normal OpenHAB operation was later observed at `0xB900`: SmartGrid with charge
from PV, feed-in, both discharge-to-load paths, and AC output enabled, while AC
grid charging was disabled. At a later monitor sample the battery reported 3.0 A
charge from decimal register 769 (`0x0301`), zero discharge at decimal register
770 (`0x0302`), 49.6 V, and 95% SOC. This is consistent with PV charging being
allowed even though AC-grid charging is disabled; the source of charge must be
inferred from flags and full PV/grid telemetry, not charge current alone.
