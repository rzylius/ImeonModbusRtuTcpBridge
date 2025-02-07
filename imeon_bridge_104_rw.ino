#include "secrets.h"

#include <SimpleSyslog.h>
#include <ModbusIP_ESP8266.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include "esp_system.h"           // ESP framework
#include <esp_task_wdt.h>         // ESP watchdog
#include <EEPROM.h>               // for storing reboot counter
#include <freertos/FreeRTOS.h>    // for multitasking on dedicated cores
#include <freertos/queue.h>       // write queue management

// ---------------------- Configuration ----------------------
// Syslog server configuration
SimpleSyslog syslog(SYSLOG_NAME, HOSTNAME, SYSLOG_SERVER_IP);
#define SYSLOG_FACILITY FAC_USER
#define LOG_INFO(fmt, ...)    syslog.printf(SYSLOG_FACILITY, PRI_INFO, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   syslog.printf(SYSLOG_FACILITY, PRI_ERROR, fmt, ##__VA_ARGS__); blinkLED(LED_ERR)
#define LOG_DEBUG(fmt, ...)   syslog.printf(SYSLOG_FACILITY, PRI_DEBUG, fmt, ##__VA_ARGS__)

// Define Modbus instances
ModbusIP mbTcp;
ModbusMaster mbImeon;
ModbusIP mbBat;

IPAddress mbBatDestination(10, 0, 20, 220); // IP address of Modbus server to send battery data

// Write Queue Settings
struct WriteCommand {
    uint16_t address;            // Register starting address
    uint16_t length;             // Number of registers to write
    uint16_t values[MAX_WRITE_VALUES];  // Raw register values (each register is 2 bytes)
};

QueueHandle_t commandQueue;  // Queue handle

//--------wifi
unsigned long wifiReconnectTimer = 0;   // Timer for reconnection attempts

// Flags and timing variables for asynchronous processing
bool isRtuTransaction = false;
unsigned long transactionStartTime = 0;
unsigned long nextProcessTime = 0;
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
uint16_t rebootCounter = 0;

uint32_t tcpTime = 0;

// Predefined ranges
struct RegisterRange {
    uint16_t start;   // Starting register number
    uint16_t length;  // Length of the range
};
// registers in decimal and length, which will be queried from IMEON in round robin fashion
// and stored in mbTcp
const RegisterRange predefinedRanges[] = {
    {256, 30},
    {512, 22},
    {768, 4},
    {1024, 16},
    {1283, 6},
    {4096, 5},
    {4352, 2},
    {4864, 18},
    {4899, 1},
    {5125, 8}
};
const int rangeCount = sizeof(predefinedRanges) / sizeof(predefinedRanges[0]); // Get the size of the array
unsigned long requestStartTime = 0;
int currentRangeIndex = 0;

Modbus::ResultCode onModbusRequest(uint8_t* data, uint8_t length, void* custom) {
  blinkLED(LED_TRANS);
  uint8_t functionCode = data[0];
  auto src = (Modbus::frame_arg_t*) custom;
  uint16_t transactionId = src->transactionId;
  uint16_t address = (data[1] << 8) | data[2];
      
  tcpTime = millis();

  switch (functionCode) {
    case Modbus::FC_READ_COILS:        // read coils
    case Modbus::FC_READ_REGS:   {     //read holding regs
      LOG_DEBUG("TCPread: 0x%02X, Address: 0x%02X %d, passthrough\n", functionCode, address, address);
      return Modbus::EX_PASSTHROUGH;
    }
    case Modbus::FC_WRITE_COIL:  {      //write single coil
      uint16_t singleCoilValue = (data[3] == 0xFF) ? 1 : 0; // Only check the high byte
      uint16_t reg_val = 0; // Default value
      switch (address) {
        case 615: reg_val = singleCoilValue ? 0x8000 : 0x7FFF; break; // Bit 15
        case 614: reg_val = singleCoilValue ? 0x4000 : 0xBFFF; break; // Bit 14
        case 613: reg_val = singleCoilValue ? 0x2000 : 0xDFFF; break; // Bit 13
        case 612: reg_val = singleCoilValue ? 0x1000 : 0xEFFF; break; // Bit 12
        case 611: reg_val = singleCoilValue ? 0x0800 : 0xF7FF; break; // Bit 11
        case 610: reg_val = singleCoilValue ? 0x0400 : 0xFBFF; break; // Bit 10
        case 609: reg_val = singleCoilValue ? 0x0200 : 0xFDFF; break; // Bit 09
        case 608: reg_val = singleCoilValue ? 0x0100 : 0xFEFF; break; // Bit 08
        default: return Modbus::EX_ILLEGAL_ADDRESS; // Unknown register, return error
      }
      enqueueWriteCommand(0x1306, 1, &reg_val);
      mbTcp.Coil(PWR_ADDRESS, 0);
      mbTcp.Hreg(0x1306, UNDEF_VALUE);
      return Modbus::EX_PASSTHROUGH;                // process this request further
    }  
    case Modbus::FC_WRITE_REG: { 
      // Validate data length 
      uint16_t singleRegValue = (data[3] << 8) | data[4];
      enqueueWriteCommand(address, 1, &singleRegValue);
      LOG_DEBUG("TCPwrite: 0x%02X, Addr=%d 0x%02X, Value=%d\n", 
                    functionCode, address, address, singleRegValue); 
      
      mbTcp.Hreg(address, UNDEF_VALUE);
      if (address == 0x1306) {                //if 0x1306 register gets modbusTCP write command, set PWR_ADDRESS to 0
        mbTcp.Coil(PWR_ADDRESS, 0);
      }
      break;
    }

    case Modbus::FC_WRITE_REGS: {
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
      LOG_DEBUG("Success response sent for transaction: %d\n", src->transactionId);
  } else {
      LOG_ERROR("Failed to send TCP response.");
  }
  return Modbus::EX_SUCCESS;
}

void blinkLED(int led) {
    digitalWrite(led, HIGH);
    vTaskDelay(pdMS_TO_TICKS(50));
    digitalWrite(led, LOW);
}

// transform 0x1306 register info to coils
uint16_t cb0x1306(TRegister* reg, uint16_t val) {
  LOG_DEBUG("cb 0x1306");
  for (uint8_t bit = 8; bit < 16; bit++) {
    bool bit_value = (val & (1 << bit)) != 0; // Extract bit 8 to 15 as boolean
    mbTcp.Coil(PWR_ADDRESS + bit, bit_value); // Assign to coil indexes 0 to 7
    LOG_DEBUG("PWR_ADDRESS: %d, val: %d", PWR_ADDRESS + bit, bit_value);
  }
  mbTcp.Coil(PWR_ADDRESS, 1);
  return val;
}


// callback to send info about battery to modbusTCP server 
// I use it to send to esp32 controller which reads smartmeter readinngs
// and answers requests from heatpump
uint16_t cbBat(TRegister* reg, uint16_t val) {
    unsigned long startTime = millis();
    uint16_t result = 0;

    while (true) {
        if (mbBat.isConnected(mbBatDestination)) {
            result = mbBat.pushHreg(mbBatDestination, BAT_START_ADDRESS, BAT_START_ADDRESS, BAT_NR_ADDRESSES);
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
  //LOG_DEBUG("TCP fc: %d, time: %d\n", fc, t);
  return Modbus::EX_SUCCESS;
}

// Callback function for client connect. Returns true to allow connection.
bool cbConn(IPAddress ip) {
  Serial.printf("TCP client connected %s\n", ip.toString().c_str());
  LOG_INFO("TCP client connected %s\n", ip.toString().c_str());;
  return true;
}

uint16_t cbRebootCounter(TRegister* reg, uint16_t val) {
  // reset counter if REBOOT_COUNTER hreg set to 0
  if (val == 0) {
    rebootCounter == 0;
    EEPROM.writeUInt(EEPROM_REBOOT_COUNTER_ADDRESS, rebootCounter); // Write the updated counter back to EEPROM
    EEPROM.commit();  // Commit the changes to EEPROM (save them!)
  } else {
    val = rebootCounter; // if mbTcp sets value non zero, ignore it
  }
  return val;
}

void updateTrackingRegisters() {
  // Update metrics and tracking registers
  mbTcp.Hreg(READ_COUNT_H, highWord(readCount));
  mbTcp.Hreg(READ_COUNT_L, lowWord(readCount));
  mbTcp.Hreg(READ_ERROR, readError);
  mbTcp.Hreg(READ_TIME, readTime);
  mbTcp.Hreg(MAX_READ_TIME, maxReadTime);
  mbTcp.Hreg(WRITE_COUNT_H, highWord(writeCount));
  mbTcp.Hreg(WRITE_COUNT_L, lowWord(writeCount));
  mbTcp.Hreg(WRITE_ERROR, writeError);
  mbTcp.Hreg(WRITE_TIME, writeTime);
  mbTcp.Hreg(MAX_WRITE_TIME, maxWriteTime);
  mbTcp.Hreg(ROUND_ROBIN_TIME, roundRobinTime / 1000);
  mbTcp.Hreg(MAX_ROUND_ROBIN_TIME, maxRoundRobinTime / 1000); // max time is in seconds
  mbTcp.Hreg(WRITE_QUEUE_SIZE, uxQueueMessagesWaiting(commandQueue));
  mbTcp.Hreg(REBOOT_COUNTER, rebootCounter);
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
    LOG_ERROR("Wi-Fi disconnected. Attempting to reconnect...");
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

void modbusRTU(void* parameter) {
  WriteCommand command;
  uint8_t result = 0;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(200));

    // check if time has passed for next modbusRTU transaction
    if (!isRtuTransaction && (millis() - lastQueryTime >= READ_QUERY_INTERVAL)) {
      isRtuTransaction = true;
      transactionStartTime = millis();

      // check if anything sits in write_queue
      if (xQueueReceive(commandQueue, &command, pdMS_TO_TICKS(5)) == pdTRUE) {
        // process write request
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

        // Multiple registers write (Function Code 0x10), my testing shows that single register write 0x06 does not work
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

      // if there is nothing in write_queue, proceed with next read  
      } else {
        // select next range of registers to read
        RegisterRange currentRange = predefinedRanges[currentRangeIndex];
        currentRangeIndex = (currentRangeIndex + 1) % rangeCount; // roll register
        if (currentRangeIndex == 0) {
          roundRobinTime = millis() - startRoundRobinTime;
          if (roundRobinTime > maxRoundRobinTime) {
            maxRoundRobinTime = roundRobinTime;
          }
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
          Serial.printf("ReadRTU success: 0x%04X %d, length: %d, time: %d\n", reg, reg, length, readTime);
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
      }
      // finish modbusRTU transaction
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

  // EEPROM reboot counter
  EEPROM.begin(512); // Allocate 512 bytes of EEPROM (adjust if needed)
  rebootCounter = EEPROM.readUInt(EEPROM_REBOOT_COUNTER_ADDRESS); // Read the reboot counter from EEPROM
  Serial.printf("Reboot counter stored in eeprom: %d\n", rebootCounter);
  rebootCounter++; // Increment the counter
  EEPROM.writeUInt(EEPROM_REBOOT_COUNTER_ADDRESS, rebootCounter); // Write the updated counter back to EEPROM
  EEPROM.commit();  // Commit the changes to EEPROM (save them!)

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

  pinMode(LED_ERR, OUTPUT);
  pinMode(LED_TRANS, OUTPUT);
  
  // Initialize Modbus
  mbTcp.onConnect(cbConn);
  mbTcp.onRequest(cbPreRequest);
  mbTcp.onRequestSuccess(cbPostRequest);
  mbTcp.onRaw(onModbusRequest);           // capture all modbusTCP requests and process in callback
  mbTcp.server(); // Set ESP32 as Modbus TCP server
  mbBat.client();
  
  Serial2.begin(BAUD_RATE, SERIAL_8N1, PIN_RX, PIN_TX); // RX = 16, TX = 17
  mbImeon.begin(MODBUS_RTU_ID, Serial2);

  // Iterate through each range and print all individual registers
  for (int i = 0; i < rangeCount; ++i) {
    uint16_t start = predefinedRanges[i].start;
    uint16_t length = predefinedRanges[i].length;
    for (uint16_t reg = start; reg < start + length; ++reg) {
      mbTcp.addHreg(reg, 0); // add each register 
    }
  }

  for (uint16_t i = READ_COUNT_H; i <= REBOOT_COUNTER; i++) {
    mbTcp.addHreg(i, 0);
  }
  mbTcp.onSetHreg(771, cbBat);      // when battery last relevant registry is written, callback will  send it to remote device

  for (int i = 0; i < 16; ++i) {
    mbTcp.addCoil(PWR_ADDRESS + i, 0);
  }
  mbTcp.onSetHreg(4870, cb0x1306);

  
  // Initialize the command queue
  commandQueue = xQueueCreate(WRITE_QUEUE_LENGTH, sizeof(WriteCommand));
  if (commandQueue == NULL) {
      Serial.println("Error: Failed to create commandQueue.");
      // Handle the error  by restarting the ESP32
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

}

void loop() {
  mbTcp.task();
  vTaskDelay(pdMS_TO_TICKS(300));
  manageWiFi();
}
