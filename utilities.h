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

void writeQueueInit();
void enqueueWriteCommand(uint16_t address, uint16_t registerCount, const uint16_t* values);
void rebootCounterInit();
void rebootCounterSet(int rebootCounter);

#endif