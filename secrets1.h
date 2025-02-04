#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_SSID "TTT"
#define WIFI_PASSWD "AAA"
#define HOSTNAME "NNN"
#define SYSLOG_NAME "SSS"
#define SYSLOG_SERVER_IP "10.0.20.1"

// Modbus configurations
#define BAT_START_ADDRESS 768         // Start address for battery registers
#define BAT_NR_ADDRESSES 4            // Number of battery addresses to process
#define PWR_ADDRESS 600               // coil start base for 0x1306 registry transformation to Coils
                                      // transformation will be 600 + bit number (8 to 15)

// PINS pin definitions
#define LED_TRANS 26   // Transaction LED
#define LED_ERR 33     // Error LED
#define PIN_RX 16       /// PINs for TTL-rs485
#define PIN_TX 17
#define BAUD_RATE 9600

#define MODBUS_RTU_ID 1
#define READ_QUERY_INTERVAL 1000  // milisecs between reads of modbusRTU
#define WRITE_QUEUE_LENGTH 10  // Maximum number of commands in the queue
#define MAX_WRITE_VALUES 15       // Maximum number of registers per command

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

#endif