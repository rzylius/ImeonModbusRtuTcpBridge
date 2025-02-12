#include "modbus_tcp.h"
#include <ModbusIP_ESP8266.h>
#include <Arduino.h>
#include "logging.h"
#include "utilities.h"

ModbusIP mbTcp;
ModbusIP mbBat;

uint32_t tcpTime = 0;

// transform 0x1306 register info to coils
uint16_t cb0x1306(TRegister* reg, uint16_t val) {
  LOG_DEBUG("cb 0x1306");
  for (uint8_t bit = 8; bit < 16; bit++) {
    bool bit_value = (val & (1 << bit)) != 0; // Extract bit 8 to 15 as boolean
    mbTcp.Coil(PWR_ADDRESS + bit, bit_value); // Assign to coil indexes 0 to 7
  }
  #if ENABLE_TEMP_STATE
    mbTcp.Coil(PWR_ADDRESS, 1);
  #endif
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
    vTaskDelay(pdMS_TO_TICKS(10)); // Small delay for processing other tasks
  }
  return val;
}

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
    } break;

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
      mbTcp.Hreg(0x1306, UNDEF_VALUE);
      mbTcp.Coil(PWR_ADDRESS, 0);
      return Modbus::EX_PASSTHROUGH;                // process this request further
    } break;

    case Modbus::FC_WRITE_REG: { 
      // Validate data length 
      uint16_t singleRegValue = (data[3] << 8) | data[4];
      enqueueWriteCommand(address, 1, &singleRegValue);
      LOG_DEBUG("TCPwrite: 0x%02X, Addr=%d 0x%02X, Value=%d\n", 
                    functionCode, address, address, singleRegValue); 
      
      #if ENABLE_TEMP_STATE     
        mbTcp.Hreg(address, UNDEF_VALUE);
        if (address == 0x1306) {                //if 0x1306 register gets modbusTCP write command, set PWR_ADDRESS to 0
          mbTcp.Coil(PWR_ADDRESS, 0);
        }
      #endif
    } break;

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
    } break;

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
  if (val = 0) {
    rebootCounter = 0;
    rebootCounterSet(rebootCounter);
  } else {
    val = rebootCounter; // if mbTcp sets value non zero, ignore it
  }
  return val;
}

void modbusTcpInit(){
  // Initialize Modbus
  mbTcp.onConnect(cbConn);
  mbTcp.onRequest(cbPreRequest);
  mbTcp.onRequestSuccess(cbPostRequest);
  mbTcp.onRaw(onModbusRequest);           // capture all modbusTCP requests and process in callback
  mbTcp.server(); // Set ESP32 as Modbus TCP server
  mbBat.client();

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
}