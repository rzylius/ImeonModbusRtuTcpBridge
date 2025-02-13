#include "logging.h"
#include "secrets.h"
#include "config.h"
#include <Adafruit_NeoPixel.h>

// Initialize the global syslog object.
SimpleSyslog syslog(SYSLOG_NAME, HOSTNAME, SYSLOG_SERVER_IP);

#ifdef LED_MODE == 2
Adafruit_NeoPixel pixels(NUMPIXELS, LED_PIN, NEO_GRB + NEO_KHZ800);

// Define LED colors (using uint32_t for direct color values)
static const uint32_t colorBlue   = pixels.Color(0, 0, 255);
static const uint32_t colorRed    = pixels.Color(255, 0, 0);
static const uint32_t colorGreen  = pixels.Color(0, 255, 0);
static const uint32_t colorWhite  = pixels.Color(255, 255, 255);
static const uint32_t colorOff    = pixels.Color(0, 0, 0); // Define 'off' color

const uint32_t ledColors[] = {
  colorBlue,  // LED_TCP (index 1 - but array is 0-indexed, so we'll adjust in blinkLED)
  colorRed,   // LED_ERR (index 2)
  colorGreen, // LED_RTU_TX (index 3)
  colorWhite  // LED_RTU_TX
};
#endif

void loggingInit() {
  // Initialize Serial for debugging
  Serial.begin(115200);
  while (!Serial) {
    ; // Wait for Serial to initialize
  }

  #ifdef LED_MODE == 2
  pixels.begin(); // INITIALIZE NeoPixel strip object (REQUIRED)
  pixels.clear(); // Set all pixel colors to 'off'
  pixels.show();   // Send the updated pixel colors to the hardware.
  #elif LED_MODE == 1
  pinMode(LED_ERR, OUTPUT);
  pinMode(LED_TCP, OUTPUT);
  pinMode(LED_RTU_TX, OUTPUT);
  pinMode(LED_RTU_RX, OUTPUT);
  #endif
}

#ifdef LED_MODE == 2
// Function to blink the LED based on LED type
void blinkLED(int led) {
  pixels.setPixelColor(0, ledColors[led]);
  pixels.show();
  vTaskDelay(pdMS_TO_TICKS(50));
  pixels.setPixelColor(0, colorOff);
  pixels.show();
}
#elif == 1
void blinkLED(int led) {
    digitalWrite(led, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(led, LOW);
}
#endif