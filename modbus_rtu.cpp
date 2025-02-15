#include "modbus_rtu.h"
#include "config.h"

ModbusMaster mbImeon;

void modbusRtuInit () {
  Serial1.begin(BAUD_RATE, SERIAL_8N1, PIN_RX, PIN_TX); // RX = 16, TX = 17
  mbImeon.begin(MODBUS_RTU_ID, Serial1);

}