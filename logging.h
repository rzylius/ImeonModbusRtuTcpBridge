#ifndef LOGGING_H
#define LOGGING_H

#include <SimpleSyslog.h>

// Declare the syslog object so it can be shared throughout the project
extern SimpleSyslog syslog;

// Set the syslog facility
#define SYSLOG_FACILITY FAC_USER

// Logging macros. Using a do-while(0) block for safety.
#define LOG_INFO(fmt, ...)    do { syslog.printf(SYSLOG_FACILITY, PRI_INFO, fmt, ##__VA_ARGS__); } while(0)
#define LOG_DEBUG(fmt, ...)   do { syslog.printf(SYSLOG_FACILITY, PRI_DEBUG, fmt, ##__VA_ARGS__); } while(0)

#if LED_MODE == 0
#define LOG_ERROR(fmt, ...)   do { syslog.printf(SYSLOG_FACILITY, PRI_ERROR, fmt, ##__VA_ARGS__); } while(0)
#elif LED_MODE == 1 || LED_MODE == 2
#define LOG_ERROR(fmt, ...)   do { syslog.printf(SYSLOG_FACILITY, PRI_ERROR, fmt, ##__VA_ARGS__); blinkLED(LED_ERR); } while(0)
#endif

void blinkLED(int led);
void loggingInit();

#endif