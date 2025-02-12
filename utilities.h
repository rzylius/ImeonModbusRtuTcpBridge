#ifndef UTILITIES_H
#define UTILITIES_H

#include <freertos/FreeRTOS.h>    // for multitasking on dedicated cores
#include <freertos/queue.h>       // write queue management
#include "config.h"

extern uint16_t rebootCounter;
extern QueueHandle_t commandQueue; 
struct WriteCommand {
  uint16_t address;            // Register starting address
  uint16_t length;             // Number of registers to write
  uint16_t values[MAX_WRITE_VALUES];  // Raw register values (each register is 2 bytes)
};

// Metrics tracking variables
extern uint32_t readCount;
extern uint16_t readError;
extern uint16_t readTime;
extern uint16_t maxReadTime;
extern uint32_t writeCount;      // Tracks total write requests
extern uint16_t writeError;         // Tracks write errors
extern uint16_t writeTime;    // 
extern uint16_t maxWriteTime;    // Max response time for write requests
extern uint16_t roundRobinTime;
extern uint16_t maxRoundRobinTime;    // Max time to process all set reads
extern uint32_t startRoundRobinTime;      // 
extern uint16_t writeQueueCount;


void writeQueueInit();
void enqueueWriteCommand(uint16_t address, uint16_t registerCount, const uint16_t* values);
void rebootCounterInit();
void rebootCounterSet(int rebootCounter);
void updateTrackingRegisters();

#endif