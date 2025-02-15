#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"  // Contains your WiFi credentials
#include "logging.h"

static unsigned long wifiReconnectTimer = 0;

void connectWiFi() {
  // Initialize WiFi
  Serial.println("Connecting to Wi-Fi...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWD);

  // Allow time for the initial connection
  unsigned long startTime = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < 10000) {
      Serial.print(".");
      delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
      LOG_INFO("Connected to Wi-Fi. IP Address: %s\n", WiFi.localIP().toString().c_str());
  } else {
      LOG_ERROR("\nInitial Wi-Fi connection failed. Managing reconnection...\n");
  }

  wifiReconnectTimer = millis(); // Initialize the reconnection timer
}

// Function to connect or reconnect to Wi-Fi
void manageWiFi() {
    static bool logsSent = false;
    static unsigned long reconnectAttempts = 0;
    const unsigned long reconnectDelay[] = {5000, 10000, 20000, 40000, 80000}; // in milliseconds

    if (WiFi.status() == WL_CONNECTED) {
        reconnectAttempts = 0; // Reset attempts on successful connection
        return;
    }
    LOG_ERROR("Wi-Fi disconnected. Attempting to reconnect...\n");
    // Start or continue the reconnection timer
    if (wifiReconnectTimer == 0) {
        wifiReconnectTimer = millis();
    }
    unsigned long elapsedTime = millis() - wifiReconnectTimer;
    if (reconnectAttempts < sizeof(reconnectDelay)/sizeof(reconnectDelay[0]) && elapsedTime > reconnectDelay[reconnectAttempts]) {
        WiFi.disconnect();
        WiFi.begin(WIFI_SSID, WIFI_PASSWD);
        reconnectAttempts++;
        wifiReconnectTimer = millis(); // Reset timer after an attempt
    }

    if (reconnectAttempts >= sizeof(reconnectDelay)/sizeof(reconnectDelay[0])) {
        LOG_ERROR("Reconnection failed. Rebooting...\n");
        delay(1000); // Allow time for the log message to be sent
        ESP.restart(); // Reboot the ESP32
    }
}