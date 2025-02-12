#include "utilities.h"
#include "logging.h"
#include <EEPROM.h>               // for storing reboot counter

QueueHandle_t commandQueue = NULL; 
uint16_t rebootCounter = 0;

void writeQueueInit() {
  // Initialize the command queue
  commandQueue = xQueueCreate(WRITE_QUEUE_LENGTH, sizeof(WriteCommand));
  if (commandQueue == NULL) {
      Serial.println("Error: Failed to create commandQueue.");
      // Handle the error  by restarting the ESP32
      ESP.restart();
  } else {
      Serial.println("commandQueue successfully created.");
  }
}

void enqueueWriteCommand(uint16_t address, uint16_t registerCount, const uint16_t* values) {
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

void rebootCounterInit() {
  // EEPROM reboot counter
  EEPROM.begin(512); // Allocate 512 bytes of EEPROM (adjust if needed)
  rebootCounter = EEPROM.readUInt(EEPROM_REBOOT_COUNTER_ADDRESS); // Read the reboot counter from EEPROM
  rebootCounter++; // Increment the counter
  rebootCounterSet(rebootCounter);
  Serial.printf("Reboot counter stored in eeprom: %d\n", rebootCounter);
}

void rebootCounterSet(int rebootCounter) {
  EEPROM.writeUInt(EEPROM_REBOOT_COUNTER_ADDRESS, rebootCounter); // Write the updated counter back to EEPROM
  EEPROM.commit();  // Commit the changes to EEPROM (save them!)
}
