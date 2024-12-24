#include "secrets.h"

//const char *ssid = "***";
//const char *password = "***";
//const char *hostname = "***";
//const char *syslog_name = "***";

#include <SimpleSyslog.h>
#include <ModbusIP_ESP8266.h>
#include <ModbusMaster.h>
#include <WiFi.h>

// ---------------------- Configuration ----------------------
// Syslog server configuration
SimpleSyslog syslog(SYSLOG_NAME, HOSTNAME, SYSLOG_SERVER_IP);
#define SYSLOG_FACILITY FAC_USER
#define LOG_INFO(fmt, ...)    syslog.printf(SYSLOG_FACILITY, PRI_INFO, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   syslog.printf(SYSLOG_FACILITY, PRI_ERROR, fmt, ##__VA_ARGS__); blinkErrorLED()
#define LOG_DEBUG(fmt, ...)   syslog.printf(SYSLOG_FACILITY, PRI_DEBUG, fmt, ##__VA_ARGS__)

// Modbus configurations
const uint8_t slaveId = 1;                    // IMEON ModbusRTU Slave ID
const uint16_t batStartAddress = 768;         // Start address for battery registers
const uint16_t batNrAddresses = 4;            // Number of battery addresses to process

// Define Modbus instances
ModbusIP mbTcp;
ModbusMaster mbImeon;
ModbusIP mbBat;
IPAddress mbBatDestination(10, 0, 20, 220); // IP address of Modbus server to send battery data

// Write Queue Settings
#define MAX_WRITE_VALUES 15  // Maximum number of registers per command
struct WriteCommand {
    uint16_t address;            // Register starting address
    uint16_t length;             // Number of registers to write
    uint16_t values[MAX_WRITE_VALUES];  // Raw register values (each register is 2 bytes)
};
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#define QUEUE_LENGTH 10  // Maximum number of commands in the queue
QueueHandle_t commandQueue;  // Queue handle

//--------wifi
unsigned long wifiReconnectTimer = 0;   // Timer for reconnection attempts
const unsigned long maxReconnectTime = 30000; // Maximum reconnection time (30 seconds)

// Flags and timing variables for asynchronous processing
bool isRtuTransaction = false;
unsigned long transactionStartTime = 0;
unsigned long nextProcessTime = 0;
const unsigned long QUERY_INTERVAL = 1000; // time between rtu transactions
unsigned long lastQueryTime = 0;

// Metrics tracking variables
uint32_t readCount = 0;
uint16_t readError = 0;
uint16_t readTime = 0;
uint16_t maxReadTime = 0;

uint32_t writeCount = 0;      // Tracks total write requests
uint16_t writeError = 0;         // Tracks write errors
uint16_t writeTime = 0;    // 
uint16_t maxWriteTime = 0;    // Max response time for write requests
uint16_t roundRobinTime = 0;
uint16_t maxRoundRobinTime = 0;    // Max time to process all set reads
uint32_t startRoundRobinTime = 0;      // 
uint16_t statusWifi = 0;

uint32_t tcpTime = 0;

// LED pin definitions
#define LED_TRANS 2   // Transaction LED
#define LED_ERR 4     // Error LED

// Predefined ranges
struct RegisterRange {
    uint16_t start;   // Starting register number
    uint16_t length;  // Length of the range
};
const RegisterRange predefinedRanges[] = {
    {256, 32},
    {512, 22},
    {768, 8},
    {1024, 20},
    {1283, 8},
    {4096, 8},
    {4352, 8},
    {4864, 18},
    {4899, 4},
    {5125, 8}
};
const int rangeCount = sizeof(predefinedRanges) / sizeof(predefinedRanges[0]); // Get the size of the array
unsigned long requestStartTime = 0;
int currentRangeIndex = 0;

void blinkErrorLED() {
    digitalWrite(LED_ERR, HIGH);
    vTaskDelay(pdMS_TO_TICKS(10));
    digitalWrite(LED_ERR, LOW);
}

Modbus::ResultCode onModbusRequest(uint8_t* data, uint8_t length, void* custom) {
  uint8_t functionCode = data[0];
  auto src = (Modbus::frame_arg_t*) custom;
  uint16_t transactionId = src->transactionId;
  uint16_t address = (data[1] << 8) | data[2];
      
  tcpTime = millis();

  switch (functionCode) {
    case 0x03:
      Serial.printf("TCPread: 0x%02X, Address: 0x%02X %d, passthrough\n", functionCode, address, address);
      return Modbus::EX_PASSTHROUGH;

    case 0x06: { 
      // Validate data length 
      uint16_t singleRegValue = (data[3] << 8) | data[4];
      enqueueWriteCommand(address, 1, &singleRegValue);
      LOG_DEBUG("TCPwrite: 0x%02X, Addr=%d 0x%02X, Value=%d\n", 
                    functionCode, address, address, singleRegValue); 
      break;
    }

    case 0x10: {
      // Validate data length
      uint16_t regs = (data[3] << 8) | data[4];
      uint8_t byteCount = data[5];
      
      uint16_t registerValues[regs];
      for (int i = 0; i < regs; i++) {
        registerValues[i] = (data[6 + i*2] << 8) | data[6 + i*2 + 1];
        Serial.printf(" 0x%04X ", registerValues[i]);
      }
      Serial.println("");
      enqueueWriteCommand(address, regs, registerValues);
      break;
    }

    default: 
      Serial.printf("TCP illegal function received: %02X\n", functionCode);
      LOG_ERROR("TCP illegal function received: %02X\n", functionCode);
      return Modbus::EX_ILLEGAL_FUNCTION;
  }  

  // Construct affirmative response PDU
  uint8_t response[5]; // Function Code + Starting Address (2) + Quantity of Registers (2)
  response[0] = data[0];                   // Function Code (0x10)
  response[1] = data[1];                   // Starting Address High Byte
  response[2] = data[2];                   // Starting Address Low Byte
  response[3] = data[3];                   // value of register
  response[4] = data[4];                   // value of register
  // Instantly respond with success to the client
  mbTcp.setTransactionId(src->transactionId);
  uint16_t result = mbTcp.rawResponce(
      IPAddress(src->ipaddr),  // Client IP
      response,                // Response payload
      5,                       // Length of response payload (Unit ID + Function Code + Address + Quantity)
      src->unitId              // Unit Identifier
  );
  if (result) {
      LOG_DEBUG("Success response sent for transaction: %d with result: %d\n", src->transactionId, result);
  } else {
      LOG_ERROR("Failed to send response.");
  }
  return Modbus::EX_SUCCESS;
}

uint16_t cbBat(TRegister* reg, uint16_t val) {
    unsigned long startTime = millis();
    uint16_t result = 0;

    while (true) {
        if (mbBat.isConnected(mbBatDestination)) {
            result = mbBat.pushHreg(mbBatDestination, batStartAddress, batStartAddress, batNrAddresses);
            mbBat.task();

            if (result) {
                LOG_INFO("Battery data transmitted successfully");
                return val;
            } else {
                LOG_ERROR("Failed to transmit battery data");
                break;
            }
        } else {
            mbBat.connect(mbBatDestination);
        }

        // Break the loop if the maximum allowed time is exceeded
        if (millis() - startTime >= 100) {
            LOG_ERROR("Connection timeout while transmitting battery data");
            break;
        }

        vTaskDelay(pdMS_TO_TICKS(10)); // Small delay for processing other tasks
    }

    return val;
}

Modbus::ResultCode cbPreRequest(Modbus::FunctionCode fc, const Modbus::RequestData data) {
  //LOG_DEBUG("PRE Function: %02X\n", fc);
  return Modbus::EX_SUCCESS;
}

Modbus::ResultCode cbPostRequest(Modbus::FunctionCode fc, const Modbus::RequestData data) {
  uint16_t t = millis() - tcpTime;
  LOG_DEBUG("TCP fc: %d, time: %d\n", fc, t);
  return Modbus::EX_SUCCESS;
}

// Callback function for client connect. Returns true to allow connection.
bool cbConn(IPAddress ip) {
  Serial.printf("TCP client connected %s\n", ip.toString().c_str());
  LOG_INFO("TCP client connected %s\n", ip.toString().c_str());;
  return true;
}


void updateTrackingRegisters() {
  // Update metrics and tracking registers
  mbTcp.Hreg(37100, lowWord(readCount));
  mbTcp.Hreg(37101, highWord(readCount));
  mbTcp.Hreg(37102, readError);
  mbTcp.Hreg(37103, readTime);
  mbTcp.Hreg(37104, maxReadTime);
  mbTcp.Hreg(37105, lowWord(writeCount));
  mbTcp.Hreg(37106, highWord(writeCount));
  mbTcp.Hreg(37107, writeError);
  mbTcp.Hreg(37108, writeTime);
  mbTcp.Hreg(37109, maxWriteTime);
  mbTcp.Hreg(37110, roundRobinTime);
  mbTcp.Hreg(37111, maxRoundRobinTime);
  mbTcp.Hreg(37112, uxQueueMessagesWaiting(commandQueue));
}


//----------------------Queue commands-------------------------------------------
void enqueueWriteCommand(uint16_t address, uint16_t registerCount, const uint16_t* values) {
  WriteCommand command;
  command.address = address;
  command.length = registerCount; // store number of registers
  memcpy(command.values, values, registerCount * sizeof(uint16_t));
  // Send to queue
  // Enqueue the command
  if (xQueueSend(commandQueue, &command, portMAX_DELAY) == pdTRUE) {
    LOG_DEBUG("ENQ Enqueued Address: 0x%04X, RegNum: %d, Val(s): ", address, registerCount);
    for (uint16_t i = 0; i < registerCount; i++) {
        LOG_DEBUG(" 0x%04X", values[i]);
    }
    Serial.println();
  } else {
    Serial.println("Failed to enqueue command.");
    LOG_ERROR("Failed to enqueue command.");
  }
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

    // Optional: Add a cap to prevent too many attempts
}

void modbusRTU(void* parameter) {
  WriteCommand command;
  uint8_t result = 0;

  while (true) {
    // Check if it's time to write to registers
    vTaskDelay(pdMS_TO_TICKS(200));
    Serial.print(":");
    if (!isRtuTransaction && (millis() - lastQueryTime >= QUERY_INTERVAL)) {
      Serial.println("RTU start read/write");
      // Wait for a command to be available
      isRtuTransaction = true;
      transactionStartTime = millis();
      if (xQueueReceive(commandQueue, &command, pdMS_TO_TICKS(5)) == pdTRUE) {
        // Log command details
        Serial.printf("  DEQ: Processing: Address=0x%04X, RegNum=%d, Val(s)",
                      command.address, command.length);
        LOG_INFO("  DEQ: Processing: Address=0x%04X %d, RegNum=%d, Val(s)",
                      command.address, command.address, command.length);
        for (int i = 0; i < command.length; i++) {
          uint16_t value = command.values[i];
          LOG_DEBUG(": 0x%04X %d", value, value);
          Serial.printf(": 0x%04X", value);
          mbImeon.setTransmitBuffer(i, value); // set buffer for transmission
        }
        Serial.printf("\n");

        // Multiple registers write (Function Code 0x10)
        result = mbImeon.writeMultipleRegisters(command.address, command.length);
        
        // Check the result
        if (result == 0) {
          writeCount++;
          writeTime = millis() - transactionStartTime;
          if (maxWriteTime < writeTime) {
            maxWriteTime = writeTime;
          }
          Serial.printf("  SUCCESS: Written %d register(s) starting at 0x%04X %d, time: %d\n", 
            command.length, command.address, command.address, writeTime);
          LOG_INFO("  SUCCESS: Written %d register(s) starting at 0x%04X %d, time: %d\n", 
            command.length, command.address, command.address, writeTime);
        } else {
          Serial.printf("  ERROR: Write failed with code 0x%02X, requeueing\n", result);
          LOG_ERROR("  ERROR: Write failed with code 0x%02X, requeueing\n", result);
          writeError++;
          // Re-enqueue the failed command
          enqueueWriteCommand(command.address, command.length, command.values); // Use enqueueWriteCommand here
          }
        }
      } else {
        // when writeQueue is empty go on reading registers
        // select next range of registers to read
        RegisterRange currentRange = predefinedRanges[currentRangeIndex];
        currentRangeIndex = (currentRangeIndex + 1) % rangeCount; // roll register
        if (currentRangeIndex == 0) {
          roundRobinTime = millis() - startRoundRobinTime;
          if (roundRobinTime > maxRoundRobinTime) {
            maxRoundRobinTime = roundRobinTime;
          }
          Serial.printf("Round robin completed, time: %d, maxTime: %d", roundRobinTime, maxRoundRobinTime);
          LOG_INFO("Round robin completed, time: %d, maxTime: %d", roundRobinTime, maxRoundRobinTime);
          startRoundRobinTime = millis();
        }
        uint16_t reg = currentRange.start;
        uint16_t length = currentRange.length;
        //Serial.printf("Read range %d length: %d", reg, length);
        result = mbImeon.readHoldingRegisters(reg, length);
        
        if (result == 0) {
          readCount++;
          readTime = millis() - transactionStartTime;
          if (maxReadTime < readTime) {
            maxReadTime = readTime;
          }
          LOG_DEBUG("ReadRTU success: 0x%04X %d, length: %d, time: %d :: ", reg, reg, length, readTime);
          Serial.printf("ReadRTU success: 0x%04X %d, length: %d, time: %d :: ", reg, reg, length, readTime);
          // Iterate through the response and print register values
          for (uint16_t i = 0; i < length; i++) {
            uint16_t value = mbImeon.getResponseBuffer(i);
            //LOG_DEBUG(" %d : %d ", reg + i, value);
            mbTcp.Hreg(reg + i, value);
          }
        } else {
          // Handle errors
          LOG_ERROR("ReadRTU error: reg %d, length: %d, result: %02X\n", reg, length, result);
          readError++;
      }
      lastQueryTime = millis();
      isRtuTransaction = false;
      updateTrackingRegisters();
    }
  }
}


void setup() {
    // Initialize Serial for debugging
  Serial.begin(115200);
  while (!Serial) {
    ; // Wait for Serial to initialize
  }

  pinMode(LED_ERR, OUTPUT);
  pinMode(LED_TRANS, OUTPUT);

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
  } else {
      Serial.println("\nInitial Wi-Fi connection failed. Managing reconnection...");
  }

  wifiReconnectTimer = millis(); // Initialize the reconnection timer
  
  // Initialize Modbus
  mbTcp.onConnect(cbConn);
  mbTcp.onRequest(cbPreRequest);
  mbTcp.onRequestSuccess(cbPostRequest);
  mbTcp.onRaw(onModbusRequest);
  mbTcp.server(); // Set ESP32 as Modbus TCP server
  mbBat.client();
  
  Serial2.begin(9600, SERIAL_8N1, 16, 17); // RX = 16, TX = 17
  mbImeon.begin(1, Serial2);

  // Iterate through each range and print all individual registers
  for (int i = 0; i < rangeCount; ++i) {
    uint16_t start = predefinedRanges[i].start;
    uint16_t length = predefinedRanges[i].length;
    for (uint16_t reg = start; reg < start + length; ++reg) {
      mbTcp.addHreg(reg, 0); // add each register 
    }
  }
  for (uint16_t i = 37100; i <= 37115; i++) {
    mbTcp.addHreg(i, 0);
  }
  mbTcp.onSetHreg(771, cbBat);

  // Initialize the command queue
  commandQueue = xQueueCreate(QUEUE_LENGTH, sizeof(WriteCommand));
  if (commandQueue == NULL) {
      Serial.println("Error: Failed to create commandQueue.");
      // Handle the error appropriately, possibly by restarting the ESP32
      ESP.restart();
  } else {
      Serial.println("commandQueue successfully created.");
  }
  // Create the processing task
  xTaskCreatePinnedToCore(
      modbusRTU,                        // Task function
      "ModbusRTUreadWriteCommandTask",  // Name of the task
      8192,                             // Stack size in words (4096 bytes)
      NULL,                             // Task input parameter
      1,                                // Priority
      NULL,                             // Task handle
      1                                 // Core ID (0 for core 0)
  );

  // Example: Enqueue a few commands
  //uint8_t values1[] = {0x12, 0x34, 0x56, 0x78};  // Two registers (0x1234, 0x5678)
  //enqueueWriteCommand(0x0010, 4, values1);

  //uint8_t values2[] = {0x9A, 0xBC};  // One register (0x9ABC)
  //enqueueWriteCommand(0x0020, 2, values2);
}

void loop() {
  mbTcp.task();
  vTaskDelay(pdMS_TO_TICKS(300));
  manageWiFi();
}