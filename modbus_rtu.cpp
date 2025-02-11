#include <Arduino.h>
#include <string.h>  // for memcpy
#include "modbus_rtu.h"
#include "log_utils.h"

void modbusRTU_enqueueWriteCommand(uint16_t address, uint16_t registerCount, const uint16_t* values) {
  WriteCommand command;
  command.address = address;
  command.length = registerCount; // store number of registers
  memcpy(command.values, values, registerCount * sizeof(uint16_t));
  // Send to queue
  // Enqueue the command
  if (xQueueSend(commandQueue, &command, portMAX_DELAY) == pdTRUE) {
    LOG_DEBUG("ENQ Enqueued Address: 0x%04X, RegNum: %d, Val(s): ", address, registerCount);
    for (uint16_t i = 0; i < registerCount; i++) {
      LOG_DEBUG(" 0x%04X", values[i]);
    }
  } else {
    Serial.println("Failed to enqueue command.");
    LOG_ERROR("Failed to enqueue command.");
  }
}