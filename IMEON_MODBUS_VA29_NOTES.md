# IMEON Modbus vA.29 engineering notes

This is a working digest of `Modbus.communication.protocol.-.RW.-.EN.vA.29.pdf`
for development and testing of this bridge. The source document is marked
restricted distribution and remains authoritative. These notes deliberately
summarize rather than reproduce it.

## Applicability and safety

- Applies to IMEON 3.6 and IMEON 9.12, with inverter software 1.7.8.2 or newer.
- The document identifies 9.12 serial numbers as beginning with `8809111`.
- Modbus must be activated once at communication startup by writing the
  IMEON-provided eight-byte key to `0x9000` (four words).
- Settings written through Modbus have the same priority as OS.ONE changes and
  are processed in arrival order.
- Written settings are volatile: an inverter restart restores the prior saved
  settings, and Modbus changes are not shown in OS.ONE.
- Commands can affect grid connection, battery limits, relays, operating mode,
  power flow, and inverter restart. Tests must not write arbitrary values.

## RTU transport rules

| Property | Required value |
|---|---:|
| Slave address | `0x01` |
| Baud rate | 9600 |
| Framing | 8 data bits, no parity, 1 stop bit (8N1) |
| Register/data byte order | Big-endian (MSB first) |
| CRC byte order | Little-endian (low byte first) |
| Response timeout | 10 seconds |
| Minimum interval between reads | Greater than 500 ms |
| Minimum interval between writes | Greater than 1 second |

Instantaneous values refresh in the inverter database about every 10 seconds;
set values refresh about every minute. A read immediately after a write is not
necessarily authoritative.

The recommended RS485 connection uses RJ45 pin 1 for A, pin 2 for B, and pin 8
for ground. The document recommends category 6A stranded cable and describes
10–20 m as the normal maximum range, depending on cable.

## Function and response semantics

- `0x03` reads one or more contiguous holding-register words.
- Standard `0x10` writes multiple words and returns the address and word count.
  This acknowledges the Modbus request but does **not** communicate application
  acceptance by the inverter.
- IMEON also defines nonstandard function `0x11`, whose response adds a status:
  `0x80` means accepted, `0x00` means the request was valid but the setting was
  refused, and `0xFF` means invalid query or data.
- The bridge currently sends RTU writes through ModbusMaster function `0x10`.
  Therefore a bridge `writeCount` increment proves transport-level success, not
  that the requested setting took effect. Tests must use delayed readback where
  the register is readable, or verify behavior/telemetry for write-only commands.

## Read areas used by the bridge

| Bridge range | Protocol content |
|---|---|
| `0x0100`, 30 words | AC grid measurements through `0x011D` |
| `0x0200`, 22 words | AC output measurements |
| `0x0300`, 4 words | Battery voltage, charge current, discharge current, SOC |
| `0x0400`, 16 words | PV measurements |
| `0x0503`, 6 words | Four error bytes and eight warning bytes |
| `0x1000`, 5 words | LCD timeout and inverter date/time block |
| `0x1100`, 2 words | Maximum grid-injection power |
| `0x1300`, 18 words | Battery and power-management settings through `0x1311` |
| `0x1323`, 1 word | Inverter running mode |
| `0x1405`, 8 words | Battery cut-off/re-discharge voltage and SOC blocks |

The protocol also defines AC-grid power factor at `0x011E`. The hardened bridge
reads 31 words from `0x0100`, so this register is included in its cache.

Values commonly require scaling. Voltages, currents, and frequencies are often
stored in tenths; power is generally in watts; SOC is an integer percentage.
Several power values are signed 32-bit quantities spanning two words.

### Battery measurement addresses

The battery measurement block starts at hexadecimal `0x0300` (decimal 768):

| Hex address | Decimal address | Measurement | Conversion |
|---:|---:|---|---|
| `0x0300` | 768 | Battery voltage | raw value / 10 = volts |
| `0x0301` | 769 | Battery charge current | raw value / 10 = amperes |
| `0x0302` | 770 | Battery discharge current | raw value / 10 = amperes |
| `0x0303` | 771 | Battery SOC | raw value = percent |

Charge and discharge use separate non-negative registers; do not interpret one
register as a signed bidirectional current. For example, raw value `30` at
decimal register 769 means 3.0 A charging. A discharging battery instead reports
zero at 769 and its current at decimal register 770.

## Writable command groups

The branch allowlist matches the documented command start addresses and exact
word counts. For testing, group them by risk rather than treating them alike.

### Lower-risk candidates, still requiring owner-approved values

- `0x1000` (1 word): LCD sleep time (`0`, `30`, `60`, `300`, or `600` seconds).
- `0x1001` (4 words): inverter date/time.

These still alter inverter state. A read-only validation should precede any
write, and the original value should be recorded for restoration.

### Operational and battery settings

- `0x1100`: maximum grid-injection power.
- `0x1300`: battery voltage and SOC supplied by an external BMS.
- `0x1303`–`0x1305`: PV/grid charge-current and discharge-current limits.
- `0x1306`: one power-management bit operation per request.
- `0x1307`–`0x1310`: charge, cut-off, re-discharge, and SOC thresholds.
- `0x1311`–`0x1315`: power factor and grid frequency/voltage limits.
- `0x1322`: grid-compliance selection.
- `0x1323`: operating mode.
- `0x1405`, `0x1409`: grouped battery voltage/SOC thresholds.
- `0x1418`, `0x1419`: maximum average voltage and MPPT voltage range.

Do not exercise these merely to test transport. Use a value explicitly approved
for the installation and preserve all cross-register constraints.

For battery voltage settings:

```text
cut-off < re-discharge <= float <= bulk
cut-off without grid <= cut-off with grid
re-discharge without grid <= re-discharge with grid
```

When increasing thresholds, change bulk and float first, then re-discharge and
cut-off values from high to low. When decreasing, use the reverse dependency
order. Invalid combinations may be refused even if function `0x10` succeeds.

Grid limits must keep at least 1 Hz between minimum and maximum frequency and at
least 5 V between minimum and maximum voltage.

### High-risk or special-purpose writes

- `0x1316`, `0x1319`, `0x131C`, `0x131F`, and `0x140D`: meter-correction power
  control. Initialization requires repeated commands every 2–7 seconds for at
  least 10 seconds. The last command remains active for about 60 seconds.
- `0x1321`: opens/closes the Grid Input relay and can put the inverter in standby.
- `0x1324`: starts the CEI compliance auto-test; results are available after
  approximately 30 seconds.
- `0x1417`: restarts the inverter embedded operating system.
- `0x1421` and `0x1423`: targeted total/per-phase grid-connector power. The
  value lasts about one minute and should normally be refreshed every 30 seconds.

These commands are excluded from routine bridge testing unless the operator has
explicitly approved the exact action and the installation is in a safe state.

## `0x1306` power-management command

`0x1306` is read/write, but writes operate on one bit at a time rather than
replacing the complete bitfield:

| Bit | Meaning | Enable | Disable |
|---:|---|---:|---:|
| 15 | Charge battery from PV | `0x8000` | `0x7FFF` |
| 14 | Charge battery from grid | `0x4000` | `0xBFFF` |
| 13 | Feed into grid | `0x2000` | `0xDFFF` |
| 12 | Discharge to loads with PV | `0x1000` | `0xEFFF` |
| 11 | Discharge to loads without PV | `0x0800` | `0xF7FF` |
| 10 | Discharge to grid with PV | `0x0400` | `0xFBFF` |
| 9 | Discharge to grid without PV | `0x0200` | `0xFDFF` |
| 8 | AC output | `0x0100` | `0xFEFF` |

AC-output enablement can take up to 20 minutes depending on the selected grid
standard. This command is unsuitable as a quick generic write test.

## Diagnostics

- `0x0503` contains the last four error codes, one byte each.
- `0x0505` contains the last eight warning codes, one byte each.
- Zero means no fault in an entry.
- Important examples include lost smart-meter communication (97), battery
  disconnected (30 warning or 71 error), battery overvoltage (70), overheating
  (50 and detailed codes 136–140), and watchdog/software failures (16, 38–40).

Keep the PDF’s full error table available when decoding live values; the short
examples above are not exhaustive.

## Consequences for the deployment test plan

1. Confirm boot, Wi-Fi, queue creation, and continuous serial output.
2. Observe at least two complete RTU rounds; current timing must respect the
   greater-than-500-ms read interval and tolerate 10-second responses.
3. Read bridge telemetry and error/warning blocks without writing anything.
4. Confirm that signed multiword values and scaling decode plausibly.
5. Exercise malformed TCP writes and queue-full behavior without forwarding a
   valid state-changing command to RTU.
6. If a real write is required, agree on one low-risk register and exact value,
   record its original value, wait for delayed readback, and restore it.
7. Do not test relay, compliance, restart, operating-mode, grid-power,
   power-management, battery-limit, or meter-correction writes without a
   separate explicit approval for that exact command.
