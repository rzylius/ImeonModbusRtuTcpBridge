#ifndef MODBUS_TCP_H
#define MODBUS_TCP_H

#include <ModbusIP_ESP8266.h>
#include "config.h"

extern ModbusIP mbTcp;
extern ModbusIP mbBat;

void modbusTcpInit ();

#endif