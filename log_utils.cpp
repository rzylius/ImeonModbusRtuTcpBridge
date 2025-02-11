
#include <Arduino.h>
#include "log_utils.h"
#include "config.h"


// Initialize the global syslog object.
SimpleSyslog syslog(SYSLOG_NAME, HOSTNAME, SYSLOG_SERVER_IP);

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
uint16_t rebootCounter = 0;
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

void blinkLED(int led) {
    digitalWrite(led, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(led, LOW);
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