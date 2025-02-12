#include "wifi_manager.h"
#include <Arduino.h>
#include <WiFi.h>
#include "secrets.h"  // Contains your WiFi credentials
#include "modbus_tcp.h"
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
      Serial.printf("\nConnected to Wi-Fi. IP Address: %s\n", WiFi.localIP().toString().c_str());
      LOG_INFO("\nConnected to Wi-Fi. IP Address: %s", WiFi.localIP().toString().c_str());
  } else {
      Serial.println("\nInitial Wi-Fi connection failed. Managing reconnection...");
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
    Serial.println("Wi-Fi disconnected. Attempting to reconnect...");
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
        Serial.println("Reconnection failed. Rebooting...");
        delay(1000); // Allow time for the log message to be sent
        ESP.restart(); // Reboot the ESP32
    }
}