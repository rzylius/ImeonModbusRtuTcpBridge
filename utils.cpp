

#include "utils.h"

void blinkLED(int led) {
    digitalWrite(led, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(led, LOW);
}