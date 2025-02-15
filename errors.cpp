
// modbus_error_handling.cpp
#include "errors.h"



// for modbusMaster
const char* modbusRTUErrorToString(uint8_t result) {
  switch (result) {
    case 0x01: return "Illegal Function";
    case 0x02: return "Illegal Data Address";
    case 0x03: return "Illegal Data Value";
    case 0x04: return "Slave Device Failure";
    case 0x00: return "Success";
    case 0xE0: return "Invalid Slave ID";
    case 0xE1: return "Invalid Function";
    case 0xE2: return "Response Timed Out";
    case 0xE3: return "Invalid CRC";
    default: return "Unknown RTU Error";
  }
}
