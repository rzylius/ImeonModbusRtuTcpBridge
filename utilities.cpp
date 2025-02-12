#include "utilities.h"
#include "logging.h"
#include <EEPROM.h>               // for storing reboot counter
#include "esp_system.h"           // ESP framework
#include <esp_task_wdt.h>         // ESP watchdog
#include <ModbusMaster.h>
#include "modbus_tcp.h"


QueueHandle_t commandQueue = NULL; 
uint16_t rebootCounter = 0;

// Metrics tracking variables
uint32_t readCount = 0;
uint16_t readError = 0;
uint16_t readTime = 0;
uint16_t maxReadTime = 0;
uint32_t writeCount = 0;      // Tracks total write requests
uint16_t writeError = 0;         // Tracks write errors
uint16_t writeTime = 0;    // 
uint16_t maxWriteTime = 0;    // Max response time for write requests
uint16_t roundRobinTime = 0;
uint16_t maxRoundRobinTime = 0;    // Max time to process all set reads
uint32_t startRoundRobinTime = 0;      // 
uint16_t writeQueueCount = 0;

void updateTrackingRegisters() {
  // Update metrics and tracking registers
  mbTcp.Hreg(READ_COUNT_H, highWord(readCount));
  mbTcp.Hreg(READ_COUNT_L, lowWord(readCount));
  mbTcp.Hreg(READ_ERROR, readError);
  mbTcp.Hreg(READ_TIME, readTime);
  mbTcp.Hreg(MAX_READ_TIME, maxReadTime);
  mbTcp.Hreg(WRITE_COUNT_H, highWord(writeCount));
  mbTcp.Hreg(WRITE_COUNT_L, lowWord(writeCount));
  mbTcp.Hreg(WRITE_ERROR, writeError);
  mbTcp.Hreg(WRITE_TIME, writeTime);
  mbTcp.Hreg(MAX_WRITE_TIME, maxWriteTime);
  mbTcp.Hreg(ROUND_ROBIN_TIME, roundRobinTime / 1000);
  mbTcp.Hreg(MAX_ROUND_ROBIN_TIME, maxRoundRobinTime / 1000); // max time is in seconds
  mbTcp.Hreg(WRITE_QUEUE_SIZE, writeQueueCount);
  mbTcp.Hreg(REBOOT_COUNTER, rebootCounter);
}


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
