```markdown


## Why modbus?

IMEON released json management protocol implemented on ethernet.
But it has limited functionality in comparison to modbus (e.g. json
can not command to discharge battery to grid, etc.)

So it seems modbus implementation is more versatile and reliable.


# ESP32 Modbus RTU-TCP Gateway for IMEON 9.12

This project implements an ESP32-based Modbus gateway that facilitates
communication between a Modbus TCP network and a Modbus RTU device
(specifically an IMEON solar inverter). IMEON inverter has a RTU timeout
of 10sec, so I find it not optimal to implement synchronuous RTU-TCP bridge
(as in this case your TCP requests would experiencethe same 10sec timeouts).

## Logic
- esp32 rotates predefined list of registers/length reads them from IMEON RTU and
  stores in esp32 local registers. When delay between requests is set to 1 sec,
  my round of reads is completed in ~16secs.
  You can adjust timing between reads with QUERY_INTERVAL
- TCP requests are answered instantly by esp32 from local registers
  (local registers are updated by read routine).
- TCP write requests receive immediate response SUCCESS, and are stored in
  the write queue
- esp32 processes the write queue as priority - - reading stops until write queue is empty
- if write requests fails, write request is written in the write queue again (tbd)


## Features

* **Metrics Tracking:**  Monitors and stores performance metrics such as read/write
    counts, errors, and timing information, accessible via Modbus registers.
* **Syslog Integration:**  Utilizes Syslog for logging events and errors to a remote server
    for debugging and monitoring.
* **WiFi Reconnection:**  Includes a mechanism to automatically reconnect to Wi-Fi in case of disconnection.

## Metrics

Metrics for monitoring health of esp32 operations:

* 37100 uint32, number of validated RTU ranges queued for cache publication
* 37102 uint16, number of RTU read errors
* 37103 uint16, last read operation duration in ms
* 37104 uint16, maximum read operation duration in ms
* 37105 uint32, RTU-acknowledged write operation count
* 37107 uint16, write error count
* 37108 uint16, last write operation duration in ms
* 37109 uint16, maximum write operation duration in ms
* 37110 uint16, last duration it took to read all registers in the list, in seconds
* 37111 uint16, maximum time it took to read all the registers, in seconds
* 37112 uint16, number of write commands dropped after retry exhaustion
* 37113 uint16, starting address of the last failed write command
* 37114 uint16, last ModbusMaster write result code (zero after an RTU acknowledgement)
* 37115 uint16, count of acknowledged RTU snapshots rejected by sanity validation
* 37116 uint16, starting address of the last rejected telemetry range
* 37117 uint16, validation reason (`1` battery voltage, `2` battery current,
  `3` battery SOC, `4` PV power, `5` PV voltage, `6` PV current)
* 37120 uint16, the number of write operations sitting in the write_queue
* 37121 uint16, number of times esp32 rebooted. You can set it to zero, non-zero set attempts will be ignored

## Telemetry freshness

Registers 37130–37169 contain ten four-word freshness records, in the same order
as `predefinedRanges`: grid, output, battery, PV, faults, clock, injection limit,
battery settings, operating mode, and grouped battery thresholds. Each record is:

| Offset | Meaning |
|---|---|
| +0 | Starting address of the telemetry range |
| +1 | Status: `0` never published, `1` published but stale, `3` published and fresh |
| +2, +3 | Unsigned 32-bit age in milliseconds, high word first |

Age includes time waiting in the inter-core queue and saturates at `0xFFFFFFFF`;
that value also denotes an unseen range. Fresh means age is at most
`TELEMETRY_STALE_AFTER_MS` (default 120,000 ms). The status and age are updated on
core 0 with the cached range before servicing TCP requests. Failed, rejected, or
dropped updates never refresh the range. Old values remain available for diagnosis;
control consumers must require status `3` for every range they rely on. The global
read counter counts validated, successfully queued ranges, not rejected snapshots,
and cannot establish freshness of an individual range.

## Concurrency and write retries

The RTU task never accesses the `mbTcp` register container directly. Completed
RTU ranges and metric snapshots are sent to core 0 through a FreeRTOS queue.
Core 0 applies each complete range before calling `mbTcp.task()`, keeping all
`mbTcp` access on one core and preventing clients from seeing half-updated
multi-register values.

Acknowledged battery and PV snapshots pass deliberately generous corruption
bounds before entering the TCP cache. A rejected range leaves the previous
complete cached range intact. PV values up to 20 kW and 1,000 V remain visible
for operational anomaly diagnosis; the filter is not an alarm threshold.

Syslog messages and custom TCP responses are also queued for core 0. In
particular, callbacks do not call `rawResponce()` reentrantly while
`mbTcp.task()` owns the active request frame.

Failed RTU writes are retried up to `MAX_WRITE_RETRIES` times. The RTU task keeps
the failed command in a dedicated retry slot ahead of newer commands, preserving
write order even when the incoming queue fills. A telemetry read is forced
between retry attempts. Exhausted commands are dropped and reported through
registers 37112-37114. Register 37120 includes the pending retry as well as queued
commands (up to `WRITE_QUEUE_LENGTH + 1`, excluding an in-flight transaction).

Run `python3 tests/run_tests.py` for deterministic host regression tests. These
compile the actual sketch against simulated RTU, clock, and FreeRTOS queue
interfaces; they do not connect to an inverter. Tests cover ordered retries,
queue saturation during a failed transaction, and progress after exhaustion.

The TCP task runs every `TCP_TASK_INTERVAL` milliseconds. LED pulses are queued
and serviced asynchronously, so request handling and error logging do not pause
for the LED duration. Wi-Fi health checks run on their own slower interval.

## Hardware Requirements

* ESP32 (wroom or s3) development board
* TTL-RS485 board (I tested only with automatic flow control)
* IMEON solar inverter with Modbus RTU interface
* (Optional)  Syslog server


## Implementation 

I found that https://github.com/emelianov/modbus-esp8266/ does not handle well modbusRTU timeouts with IMEON,
so for modbusRTU I use modbusMaster library.

During testing I gather, that long modbusRTU timeouts interfere with the modbusTCP requests.
So I implement the following

* modbusRTU process is set to work on core1
* all other processes are set to work on core0

For this before sketch is uploaded, in Arduino Tools I set "Arduino runs on Core0" and 'Events run on Core0"


## Installation

secrets.h file is under .gitignore
So rename secrets1.h to secrets.h and set the right settings

In Arduino Tools I set "Arduino runs on Core0" and 'Events run on Core0"

### ModbusMaster timeout

IMEON protocol vA.29 specifies a response timeout of 10 seconds. The upstream
`ModbusMaster` library defaults `ku16MBResponseTimeout` to 2000 milliseconds.
Configure the library timeout to at least 10000 milliseconds; otherwise a slow
but valid inverter response will be reported as a timeout.

For ModbusMaster 2.0.1, apply both checked-in patches from the library root before
building. The second patch rejects register responses whose byte count differs
from the requested count (RTU result `0xE4`). Clearing the response buffer alone
does not fix this: it would substitute zeros for missing data.

```sh
patch -p1 < /path/to/ImeonModbusRtuTcpBridge/patches/ModbusMaster-2.0.1-imeon-timeout.patch
patch -p1 < /path/to/ImeonModbusRtuTcpBridge/patches/ModbusMaster-2.0.1-strict-read-length.patch
```

The sketch fails compilation when the strict-length patch marker is absent.
`python3 tools/build_firmware.py` copies the installed ModbusMaster 2.0.1 library,
applies any missing patches in a temporary directory, and compiles with the public
placeholder configuration. It does not modify installed libraries or upload
firmware. Pass `--modbusmaster /path/to/ModbusMaster` to select the library source.
The same option is available on `tests/run_tests.py`, which also tests the real
patched parser with complete, malformed, CRC-invalid, and recovery responses.

The bridge sends the activation key from `IMEON_ACTIVATION_KEY_BYTES` in the
ignored `secrets.h` before starting telemetry reads. The public `secrets1.h`
contains only an all-zero placeholder and must be configured locally.

### Arduino CLI and serial monitoring

The sketch uses 115200 baud for diagnostic output. First identify the USB
serial port:

```sh
arduino-cli board list
```

An ESP32 connected through native USB commonly appears as `/dev/ttyACM0`;
USB-to-serial adapters commonly appear as `/dev/ttyUSB0`. Never assume the port
name when more than one serial device is connected.

On Linux, the current user needs permission to access the port. Check the
device ownership and whether another process already has it open:

```sh
ls -l /dev/ttyACM0
fuser /dev/ttyACM0
```

Users are commonly granted serial access through the `dialout` group. A group
membership change requires a new login session before it takes effect.

To monitor interactively with Arduino CLI:

```sh
arduino-cli monitor --port /dev/ttyACM0 --config baudrate=115200
```

Opening a serial monitor may reset some ESP32 boards because the monitor can
toggle the DTR/RTS control lines. To capture a bounded log sample while
minimizing that behavior, configure the terminal with hang-up disabled and read
it through a timeout:

```sh
stty -F /dev/ttyACM0 115200 raw -echo -hupcl
timeout 30s stdbuf -oL cat /dev/ttyACM0
```

The `timeout` is important for scripts and remote debugging because a serial
read otherwise waits indefinitely when the controller is quiet. These commands
only monitor serial output; they do not compile or upload firmware.

For an ESP32 Dev Module, compile with:

```sh
arduino-cli compile \
  --fqbn esp32:esp32:esp32s3 \
  --board-options LoopCore=0,EventsCore=0 \
  /path/to/imeon_bridge_104_rw
```

Arduino requires the sketch directory and its primary `.ino` file to have the
same base name, so the directory in this example must contain
`imeon_bridge_104_rw.ino`. The required libraries are `SimpleSyslog`,
`ModbusMaster`, and `modbus-esp8266`. Keep the real `secrets.h` out of version
control and avoid printing its contents in build logs.

Uploading changes the physical controller and should be a deliberate, separate
step after confirming both the board and port:

```sh
arduino-cli upload \
  --fqbn esp32:esp32:esp32s3 \
  --board-options LoopCore=0,EventsCore=0 \
  --port /dev/ttyACM0 \
  /path/to/imeon_bridge_104_rw
```

Stop any active serial monitor before uploading because only one process can
normally own the port at a time.

### TCP write validation

The bridge accepts only writable register addresses, exact function-word counts,
and values documented by IMEON protocol vA.29. Malformed, partial, oversized,
out-of-range and undocumented commands are rejected before they reach the RTU
bus. A full write queue is reported to the TCP client as Modbus exception 0x06
instead of blocking the TCP task.

IMEON's standard function `0x10` response acknowledges RTU transport but does
not prove that the inverter applied a setting. Logs and metrics therefore use
the term "acknowledged". Confirm state-changing writes through delayed readback
or observed behavior.

### Continuous operating monitor

`tools/imeon_operation_monitor.py` records one operating snapshot per minute,
state/write-counter transitions immediately, and an hourly summary. It never
writes to or reboots the inverter. Start it from the repository root with:

```sh
python3 tools/imeon_operation_monitor.py \
  --host 10.0.20.231 \
  --log test-results/imeon-operation-monitor.jsonl \
  --status test-results/imeon-operation-status.json
```

The JSON status file is atomically replaced and can be read by OpenHAB or a
health-check process. Stop the process with `SIGTERM` so it records a
`monitor_stopped` event.

The monitor raises `suspected_stuck_grid_charge` when a TCP-originated write is
followed for ten minutes by unchanged AC-charge flags and measured charging.
This is deliberately an alert, not an automatic reboot. The current bridge
publishes its acknowledged-write count but not the last successful command's
address and value, so the monitor cannot prove that the write was specifically
the `0x1306 = 0xBFFF` disable command. A limit or mode write can otherwise look
similar. Reliable automatic recovery requires adding last-successful-write
address/value metrics or an explicit desired-state signal from OpenHAB.

Live testing showed that an IMEON reboot can leave RTU telemetry unavailable for
about seven minutes while the ESP32 and its TCP cache remain online. During this
period cached values must not be treated as current. Recovery is established
only when the bridge read counter advances through a complete polling round.
After recovery, verify mode, `0x1306`, current limits, battery currents, and
fault words before sending another command.

On the tested inverter, SmartGrid became stuck at `0xF900`: documented AC-charge
disable requests were acknowledged by both TCP and RTU but were not applied.
An inverter reboot cleared the condition. Fresh post-reboot state was `0x3900`
(feed-in and both discharge-to-load paths enabled, AC-grid charging disabled),
and the battery resumed physical discharge. Do not automate register `0x1417`
solely from the heuristic alert; require confirmed desired state and sustained
application-level mismatch first.

For quick battery-flow checks, read holding registers 768–771 (`0x0300`–
`0x0303`). Decimal register 769 is charge current and decimal register 770 is
discharge current; both have 0.1 A resolution. They are separate unsigned
measurements. A value of 30 at register 769 therefore means 3.0 A charge, not
30 A and not a signed net current.

## Peculiarities of IMEON

Overall modbusRTU is pretty reliable.

My most usual settings I use:
- discharge to grid
- discharge to loads,
- battery stay idle
- charge batteries

Are done via settings of registry 0x1306.
But sometimes "Charge battery from grid" does not work. I did not find on what it depends.
As a workaround I have script in openhab, which checks if Charge from grid selected and
battery is not charging, then it 0x1323 value 2 to switch to backup mode. Then it
charges all right. (remember to switch back to smartgrid which is value 1).

When switching between modes, the max charge / max discharge settings are reset.
So I have virtual items in openhab which hold max charge / discharge settings,
and periodically check if these value are set on IMEON. And update if they do not match.

## Contributing

Contributions are welcome! Feel free to open issues or submit pull requests for bug fixes,
improvements, or new features.

```

**Note:** Remember to replace the placeholders in the `secrets.h` file with your actual credentials.
