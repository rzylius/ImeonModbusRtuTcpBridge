#ifndef MODBUS_RTU_H
#define MODBUS_RTU_H

#include <Arduino.h>
#include <ModbusMaster.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include "config.h"

// Structure for a write command
struct WriteCommand {
    uint16_t address;                     // Starting register address
    uint16_t length;                      // Number of registers to write
    uint16_t values[MAX_WRITE_VALUES];    // Array holding register values
};

// Initialize the Modbus RTU module and start its task
void modbusRTU_init();

// Enqueue a write command for the Modbus RTU device
void modbusRTU_enqueueWriteCommand(uint16_t address, uint16_t registerCount, const uint16_t* values);

#endif // MODBUS_RTU_H
