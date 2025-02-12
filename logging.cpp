#include "logging.h"
#include "config.h"
#include "secrets.h"

// Initialize the global syslog object.
SimpleSyslog syslog(SYSLOG_NAME, HOSTNAME, SYSLOG_SERVER_IP);

void blinkLED(int led) {
    digitalWrite(led, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(led, LOW);
}