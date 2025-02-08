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

* 37100 uint32, number of RTU read operations
* 37102 uint16, number of RTU read errors
* 37103 uint16, last read operation duration in ms
* 37104 uint16, maximum read operation duration in ms
* 37105 uint32, write oprations count
* 37107 uint16, write error count
* 37108 uint16, last write operation duration in ms
* 37109 uint16, maximum write operation duration in ms
* 37110 uint16, last duration it took to read all registers in the list, in seconds
* 37111 uint16, maximum time it took to read all the registers, in seconds
* 37120 uint16, the number of write operations sitting in the write_queue
* 37121 uint16, number of times esp32 rebooted. You can set it to zero, non-zero set attempts will be ignored

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
