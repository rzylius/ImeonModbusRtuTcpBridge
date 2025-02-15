```markdown
# ESP32 Modbus RTU-TCP Gateway for IMEON 9.12

This project implements an ESP32-based Modbus gateway that facilitates communication
between a Modbus TCP network and a Modbus RTU device (specifically an IMEON solar inverter).
IMEON inverter has a RTU timeout of 10sec, so I find it not optimal to implement
synchronuous RTU-TCP bridge (as in this case your TCP requests would experience
the same 10sec timeouts).


## Logic

For reading registers:
- esp32 rotates predefined list of registers/length reads them from IMEON RTU and stores in esp32
  local registers. When delay between requests is set to 1 sec, my round of reads is completed in ~16secs.
  You can adjust timing between reads with READ_QUERY_INTERVAL in config.h
- TCP requests are answered instantly by esp32 from local registers
  (local registers are updated by read routine).

For writing requests 
- esp32 receives TCP write requests, it responds with immediate  SUCCESS. Request is stored in write queue
- esp32 processes the write queue has priority - - reading stops until write queue is empty
- if write requests fails, write request is written in the write queue again


## 0x1306 register in coils

NOTE: this functionality is enabled by the flag in config.h:

#define ENABLE_TEMP_STATE 1
  disabled with 0

There is register 0x1306 which manages power settings of IMEON.
8-15 bits of the register have separate functions, and can be changed only one by one.
So in essence they are coils, which for some reason are made in holding register.

esp32 retrieves 0x1306 register and saves it localy the same way, and as coils.
When coils are changed, it puts accordingly write request to send to IMEON

So coils works booth ways, as indication of status and as a means to change status.

Coil values
* 600 coil is status coil. It is set to 0, when write TCP request is receives (either 0x1306 or coils)
  when new status is received from IMEON, 600 set to 1
* 607 is 7 bit of 0x1306
* ...
* 615 is 15th bit of 0x1306

Process of change of 0x1306
  when tcp request changes 0x1306, or any of coils 607-615,
  0x1306 is set to 0xFFFF
  600 is set to 0
  write request is placed to write_queue (and eventually written to Imeon)
  when 0x1306 is read from Imeon, values are set to localy, coils 607-615 updated
  600 is set to 1


## Features

* **Metrics Tracking:**  Monitors and stores performance metrics such as read/write counts, errors,
    and timing information, accessible via Modbus registers.
* **Syslog Integration:**  Utilizes Syslog for logging events and errors to a remote server
    for debugging and monitoring.
* **WiFi Reconnection:**  Includes a mechanism to automatically reconnect to Wi-Fi in case of disconnection.


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

You can monitor the ESP32's activity through the Serial Monitor and check the logs on your Syslog server.


## Contributing

Contributions are welcome! Feel free to open issues or submit pull requests for bug fixes,
improvements, or new features.

```

**Note:** Remember to replace the placeholders in the `secrets.h` file with your actual credentials.
