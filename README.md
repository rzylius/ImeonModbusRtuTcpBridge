```markdown
# ESP32 Modbus RTU-TCP Gateway for IMEON 9.12

This project implements an ESP32-based Modbus gateway that facilitates communication between a Modbus TCP network
and a Modbus RTU device (specifically an IMEON solar inverter).
IMEON inverter has a RTU timeout of 10sec, so I find it not optimal to implement sunchronuous RTU-TCP bridge
(in this case your TCP requests would experience the same 10sec timeouts).


## Logic
- rotates predefined list of registers/length reads them from IMEON RTU and stores in esp32.
  When delay between requests is set to 1 sec, my round of reads is completed in ~16secs.
  You can adjust timing between reads with QUERY_INTERVAL
- TCP requests are answered instantly by esp32 (local registers are updated by read routine).
- TCP write requests receive immediate response SUCCESS, and are stored in the write queue
- esp32 processes the write queue as priority - - reading stops until write queue is empty
- if write requests fails, write request is written in the write queue again


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
