#ifndef CONFIG_H
#define CONFIG_H

#include <Arduino.h>
#include <IPAddress.h>

// Modbus configurations
#define BAT_START_ADDRESS 768         // Start address for battery registers
#define BAT_NR_ADDRESSES 4            // Number of battery addresses to process
#define PWR_ADDRESS 600               // coil start base for 0x1306 registry transformation to Coils
                                      // transformation will be 600 + bit number (8 to 15)

// PINS pin definitions
#define LED_MODE 2    // set 0 you need no leds
                     // set 1 of external pins are used for leds
                     // set 2 if you use RGB led on the esp-s3 device

#ifdef LED_MODE == 2  // if on s3 you use the onboard RGB Led
#define LED_PIN 48    // GPIO pin connected to the NeoPixel LED
#define NUMPIXELS 1   // Number of NeoPixels
#define LED_TCP 0
#define LED_ERR 1
#define LED_RTU_TX 2
#define LED_RTU_RX 3
#elif LED_MODE == 1
                        // in this case we use pins
#define LED_TCP 36      // 26 Transaction LED pin
#define LED_ERR 37      // 33 Error LED pin
#define LED_RTU_TX 38
#define LED_RTU_RX 39
#endif


// modbusRTU (serial2)
#define PIN_RX 16       /// PINs for TTL-rs485
#define PIN_TX 17
#define BAUD_RATE 9600

#define MODBUS_RTU_ID 1
#define READ_QUERY_INTERVAL 1000  // milisecs between reads of modbusRTU
#define WRITE_QUEUE_LENGTH 10  // Maximum number of commands in the queue
#define MAX_WRITE_VALUES 15       // Maximum number of registers per command

#define ENABLE_TEMP_STATE 1 // see readme file chapter "0x1306 register in coils"
#define UNDEF_VALUE 0xFFFF  // value of holding registry considered undefined

#define READ_COUNT_H 37100
#define READ_COUNT_L 37101
#define READ_ERROR 37102
#define READ_TIME 37103
#define MAX_READ_TIME 37104
#define WRITE_COUNT_H 37105
#define WRITE_COUNT_L 37106
#define WRITE_ERROR 37107
#define WRITE_TIME 37108
#define MAX_WRITE_TIME 37109
#define ROUND_ROBIN_TIME 37110
#define MAX_ROUND_ROBIN_TIME 37111  // in seconds

#define WRITE_QUEUE_SIZE 37120    // modbusRTU write commands standing in queue
                                  // counter for monitoring reboots. resets when you set it to zero
#define REBOOT_COUNTER 37121
#define EEPROM_REBOOT_COUNTER_ADDRESS 0

extern IPAddress mbBatDestination;

// Define the RegisterRange structure
// list of registers in the config.cpp
struct RegisterRange {
    int start;
    int length;
};
extern const RegisterRange predefinedRanges[];
extern const int rangeCount;

#endif