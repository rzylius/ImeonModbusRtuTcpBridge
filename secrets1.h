#ifndef SECRETS_H
#define SECRETS_H

#define WIFI_SSID "TTT"
#define WIFI_PASSWD "AAA"
#define HOSTNAME "NNN"
#define SYSLOG_NAME "SSS"
#define SYSLOG_SERVER_IP "10.0.20.1"

// mbTcp performance monitoring registers
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
#define MAX_ROUND_ROBIN_TIME 37111
// modbusRTU write commands standing in queue
#define QUEUE_SIZE 37120
// counter for monitoring reboots. resets when you set it to zero
#define REBOOT_COUNTER 37121

#define EEPROM_REBOOT_COUNTER_ADDRESS 0

#endif