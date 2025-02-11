#ifndef LOG_UTILS_H
#define LOG_UTILS_H

#include <Arduino.h>
#include <SimpleSyslog.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <EEPROM.h>               // for storing reboot counter
#include "secrets.h"

// Declare the syslog object so it can be shared throughout the project
extern SimpleSyslog syslog;

extern uint32_t readCount;
extern uint16_t readError;
extern uint16_t readTime;
extern uint16_t maxReadTime;
extern uint32_t writeCount;
extern uint16_t writeError;
extern uint16_t writeTime; 
extern uint16_t maxWriteTime;
extern uint16_t roundRobinTime;
extern uint16_t maxRoundRobinTime;
extern uint32_t startRoundRobinTime;
extern uint16_t rebootCounter;
extern uint16_t writeQueueCount;


// Set the syslog facility
#define SYSLOG_FACILITY FAC_USER

// Logging macros. Using a do-while(0) block for safety.
#define LOG_INFO(fmt, ...)    do { syslog.printf(SYSLOG_FACILITY, PRI_INFO, fmt, ##__VA_ARGS__); } while(0)
#define LOG_ERROR(fmt, ...)   do { syslog.printf(SYSLOG_FACILITY, PRI_ERROR, fmt, ##__VA_ARGS__); blinkLED(LED_ERR); } while(0)
#define LOG_DEBUG(fmt, ...)   do { syslog.printf(SYSLOG_FACILITY, PRI_DEBUG, fmt, ##__VA_ARGS__); } while(0)


void updateTrackingRegisters();
void blinkLED(int led);
void rebootCounterInit();
void rebootCounterSet(int counter);

#endif