#include "config.h"
#include "wifi_manager.h"
#include "modbus_tcp.h"
#include "logging.h"
#include "utilities.h"

#include <ModbusIP_ESP8266.h>
#include <ModbusMaster.h>
#include <WiFi.h>
#include <freertos/FreeRTOS.h>    // for multitasking on dedicated cores
#include <freertos/queue.h>       // write queue management


// Define Modbus instances

ModbusMaster mbImeon;



// Flags and timing variables for asynchronous processing
bool isRtuTransaction = false;
unsigned long transactionStartTime = 0;
unsigned long nextProcessTime = 0;
unsigned long lastQueryTime = 0;


unsigned long requestStartTime = 0;
int currentRangeIndex = 0;

void modbusRTU(void* parameter) {
  WriteCommand command;
  uint8_t result = 0;

  while (true) {
    vTaskDelay(pdMS_TO_TICKS(200));

    // check if time has passed for next modbusRTU transaction
    if (!isRtuTransaction && (millis() - lastQueryTime >= READ_QUERY_INTERVAL)) {
      isRtuTransaction = true;
      
      #if LED_MODE == 1 || LED_MODE == 2
      blinkLED(LED_RTU_TX);
      #endif

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
          
          #if LED_MODE == 1 || LED_MODE == 2
          blinkLED(LED_RTU_TX);
          #endif

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

          #if LED_MODE == 1 || LED_MODE == 2
          blinkLED(LED_RTU_RX);
          #endif

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
  
  connectWiFi();
  loggingInit();
  rebootCounterInit();
  writeQueueInit();
  modbusTcpInit();


  
 
  Serial2.begin(BAUD_RATE, SERIAL_8N1, PIN_RX, PIN_TX); // RX = 16, TX = 17
  mbImeon.begin(MODBUS_RTU_ID, Serial2);

  
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
