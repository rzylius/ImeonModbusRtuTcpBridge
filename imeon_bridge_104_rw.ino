#include "secrets.h"

#include <SimpleSyslog.h>
#include <ModbusIP_ESP8266.h>
#include <ModbusMaster.h>
#ifndef MODBUSMASTER_STRICT_READ_LENGTH
#error "Apply patches/ModbusMaster-2.0.1-strict-read-length.patch before building"
#endif
#include <WiFi.h>
#include "esp_system.h"           // ESP framework
#include <EEPROM.h>               // for storing reboot counter
#include <atomic>
#include <cstdarg>
#include <freertos/FreeRTOS.h>    // for multitasking on dedicated cores
#include <freertos/queue.h>       // write queue management

#ifndef MAX_WRITE_RETRIES
#define MAX_WRITE_RETRIES 3
#endif
#ifndef MAX_REGISTER_UPDATE_VALUES
#define MAX_REGISTER_UPDATE_VALUES 31
#endif
#ifndef REGISTER_UPDATE_QUEUE_LENGTH
#define REGISTER_UPDATE_QUEUE_LENGTH 16
#endif
#ifndef LED_QUEUE_LENGTH
#define LED_QUEUE_LENGTH 8
#endif
#ifndef LOG_QUEUE_LENGTH
#define LOG_QUEUE_LENGTH 32
#endif
#ifndef TCP_RESPONSE_QUEUE_LENGTH
#define TCP_RESPONSE_QUEUE_LENGTH 16
#endif
#ifndef LED_BLINK_DURATION
#define LED_BLINK_DURATION 50
#endif
#ifndef TCP_TASK_INTERVAL
#define TCP_TASK_INTERVAL 5
#endif
#ifndef WIFI_CHECK_INTERVAL
#define WIFI_CHECK_INTERVAL 1000
#endif
#ifndef MIN_WRITE_INTERVAL
#define MIN_WRITE_INTERVAL 1100
#endif
#ifndef METER_CONTROL_MIN_INTERVAL
#define METER_CONTROL_MIN_INTERVAL 2000
#endif
#ifndef ACTIVATION_RETRY_INTERVAL
#define ACTIVATION_RETRY_INTERVAL 5000
#endif
#ifndef ILLEGAL_FUNCTION_LOG_INTERVAL
#define ILLEGAL_FUNCTION_LOG_INTERVAL 5000
#endif
#ifndef DROPPED_WRITE_COUNT
#define DROPPED_WRITE_COUNT 37112
#endif
#ifndef LAST_FAILED_WRITE_ADDRESS
#define LAST_FAILED_WRITE_ADDRESS 37113
#endif
#ifndef LAST_WRITE_RESULT
#define LAST_WRITE_RESULT 37114
#endif
#ifndef INVALID_READ_COUNT
#define INVALID_READ_COUNT 37115
#endif
#ifndef LAST_INVALID_READ_ADDRESS
#define LAST_INVALID_READ_ADDRESS 37116
#endif
#ifndef LAST_INVALID_READ_REASON
#define LAST_INVALID_READ_REASON 37117
#endif
#ifndef TRACKING_REGISTER_COUNT
#define TRACKING_REGISTER_COUNT (REBOOT_COUNTER - READ_COUNT_H + 1)
#endif
#ifndef FRESHNESS_REGISTER_START
#define FRESHNESS_REGISTER_START 37130
#endif
#ifndef TELEMETRY_STALE_AFTER_MS
#define TELEMETRY_STALE_AFTER_MS 120000UL
#endif

void requestLedBlink(int led);
void queueLog(uint8_t priority, const char* format, ...);
QueueHandle_t logQueue = NULL;
QueueHandle_t tcpResponseQueue = NULL;

void modbusIdle() {
  // ModbusMaster otherwise busy-waits for up to the IMEON 10-second timeout,
  // starving the ESP32 idle task and triggering the task watchdog.
  vTaskDelay(pdMS_TO_TICKS(1));
}

// ---------------------- Configuration ----------------------
// Syslog server configuration
SimpleSyslog syslog(SYSLOG_NAME, HOSTNAME, SYSLOG_SERVER_IP);
#define SYSLOG_FACILITY FAC_USER
#define LOG_INFO(fmt, ...)    queueLog(PRI_INFO, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...)   do { \
    queueLog(PRI_ERROR, fmt, ##__VA_ARGS__); \
    requestLedBlink(LED_ERR); \
  } while (0)
#define LOG_DEBUG(fmt, ...)   queueLog(PRI_DEBUG, fmt, ##__VA_ARGS__)

// Define Modbus instances
ModbusIP mbTcp;
ModbusMaster mbImeon;
ModbusIP mbBat;

IPAddress mbBatDestination(10, 0, 20, 220); // IP address of Modbus server to send battery data

// Write Queue Settings
struct WriteCommand {
    uint16_t address;            // Register starting address
    uint16_t length;             // Number of registers to write
    uint8_t retryCount;          // Number of retries already attempted
    uint16_t values[MAX_WRITE_VALUES];  // Raw register values (each register is 2 bytes)
};

struct RegisterUpdate {
    uint16_t address;
    uint16_t length;
    uint32_t capturedAt;
    uint16_t values[MAX_REGISTER_UPDATE_VALUES];
};

struct LogEntry {
    uint8_t priority;
    char message[192];
};

struct TcpResponse {
    IPAddress ip;
    uint16_t transactionId;
    uint8_t unitId;
    uint8_t length;
    uint8_t payload[5];
};

static_assert(MAX_REGISTER_UPDATE_VALUES >= TRACKING_REGISTER_COUNT,
              "RegisterUpdate must fit the tracking register block");

QueueHandle_t commandQueue;
QueueHandle_t registerUpdateQueue;
QueueHandle_t ledQueue;

void queueLog(uint8_t priority, const char* format, ...) {
  LogEntry entry;
  entry.priority = priority;
  va_list args;
  va_start(args, format);
  vsnprintf(entry.message, sizeof(entry.message), format, args);
  va_end(args);

  if (logQueue != NULL) {
    xQueueSend(logQueue, &entry, 0);
  } else {
    syslog.printf(SYSLOG_FACILITY, priority, "%s", entry.message);
  }
}

void serviceLogs() {
  LogEntry entry;
  while (xQueueReceive(logQueue, &entry, 0) == pdTRUE) {
    syslog.printf(SYSLOG_FACILITY, entry.priority, "%s", entry.message);
  }
}

bool queueTcpResponse(
    Modbus::frame_arg_t* source,
    const uint8_t* payload,
    uint8_t length) {
  if (source == nullptr || payload == nullptr || length == 0 || length > 5 ||
      tcpResponseQueue == NULL) {
    return false;
  }

  TcpResponse response;
  response.ip = IPAddress(source->ipaddr);
  response.transactionId = source->transactionId;
  response.unitId = source->unitId;
  response.length = length;
  memcpy(response.payload, payload, length);
  return xQueueSend(tcpResponseQueue, &response, 0) == pdTRUE;
}

void serviceTcpResponses() {
  TcpResponse response;
  while (xQueueReceive(tcpResponseQueue, &response, 0) == pdTRUE) {
    mbTcp.setTransactionId(response.transactionId);
    if (!mbTcp.rawResponce(
            response.ip,
            response.payload,
            response.length,
            response.unitId)) {
      LOG_ERROR("Failed to send deferred TCP response");
    }
  }
}

//--------wifi
unsigned long wifiReconnectTimer = 0;   // Timer for reconnection attempts
unsigned long lastWifiCheck = 0;

// Flags and timing variables for asynchronous processing
bool isRtuTransaction = false;
unsigned long transactionStartTime = 0;
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
uint32_t roundRobinTime = 0;
uint32_t maxRoundRobinTime = 0;    // Max time to process all set reads
uint32_t startRoundRobinTime = 0;      // 
std::atomic<uint16_t> rebootCounter{0};
uint16_t droppedWriteCount = 0;
uint16_t lastFailedWriteAddress = 0;
uint16_t lastWriteResult = 0;
uint16_t invalidReadCount = 0;
uint16_t lastInvalidReadAddress = 0;
uint16_t lastInvalidReadReason = 0;
unsigned long lastWriteCompletedAt = 0;
// RTU-task-owned retry slot: never put an older command behind newer writes.
bool writeRetryPending = false;

uint32_t tcpTime = 0;

// Predefined ranges
struct RegisterRange {
    uint16_t start;   // Starting register number
    uint16_t length;  // Length of the range
};
// registers in decimal and length, which will be queried from IMEON in round robin fashion
// and stored in mbTcp
const RegisterRange predefinedRanges[] = {
    {256, 31},
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
int currentRangeIndex = 0;

// Owned by core 0. Age saturates instead of becoming fresh after millis wraps.
struct RangeFreshness {
  bool seen = false;
  uint32_t ageMs = UINT32_MAX;
};
RangeFreshness rangeFreshness[rangeCount];
uint32_t lastFreshnessTick = 0;

void ageTelemetry(uint32_t now) {
  const uint32_t elapsed = now - lastFreshnessTick;
  lastFreshnessTick = now;
  for (auto& freshness : rangeFreshness) {
    if (freshness.seen) {
      freshness.ageMs = elapsed > UINT32_MAX - freshness.ageMs
          ? UINT32_MAX : freshness.ageMs + elapsed;
    }
  }
}

void publishFreshness() {
  for (int i = 0; i < rangeCount; i++) {
    const auto& freshness = rangeFreshness[i];
    const uint16_t address = FRESHNESS_REGISTER_START + i * 4;
    const uint16_t status = !freshness.seen ? 0 :
        (freshness.ageMs <= TELEMETRY_STALE_AFTER_MS ? 3 : 1);
    mbTcp.Hreg(address, predefinedRanges[i].start);
    mbTcp.Hreg(address + 1, status);
    mbTcp.Hreg(address + 2, highWord(freshness.ageMs));
    mbTcp.Hreg(address + 3, lowWord(freshness.ageMs));
  }
}

enum InvalidReadReason : uint16_t {
  INVALID_NONE = 0,
  INVALID_BATTERY_VOLTAGE = 1,
  INVALID_BATTERY_CURRENT = 2,
  INVALID_BATTERY_SOC = 3,
  INVALID_PV_POWER = 4,
  INVALID_PV_VOLTAGE = 5,
  INVALID_PV_CURRENT = 6,
};

int32_t signed32(const uint16_t* values, uint16_t offset) {
  return static_cast<int32_t>(
      (static_cast<uint32_t>(values[offset]) << 16) | values[offset + 1]);
}

uint16_t validateTelemetryRange(
    uint16_t address,
    uint16_t length,
    const uint16_t* values) {
  if (address == 0x0300 && length >= 4) {
    const uint16_t voltageRaw = values[0];
    const uint16_t chargeCurrentRaw = values[1];
    const uint16_t dischargeCurrentRaw = values[2];
    const uint16_t soc = values[3];
    // Zero voltage is retained as a useful offline/startup indication. Values
    // between 0 and 30 V cannot represent this 48 V installation.
    if (voltageRaw != 0 && (voltageRaw < 300 || voltageRaw > 700)) {
      return INVALID_BATTERY_VOLTAGE;
    }
    // Installation limits are lower, but 250 A leaves diagnostic headroom.
    if (chargeCurrentRaw > 2500 || dischargeCurrentRaw > 2500) {
      return INVALID_BATTERY_CURRENT;
    }
    if (soc > 100) return INVALID_BATTERY_SOC;
  }

  if (address == 0x0400 && length >= 16) {
    const int32_t pv1Power = signed32(values, 0);
    const int32_t pv2Power = signed32(values, 2);
    const int32_t totalPvPower = signed32(values, 12);
    // These are corruption limits, not operational alarm thresholds. Retain
    // unusual readings up to 20 kW and 1,000 V for diagnosis.
    if (pv1Power < 0 || pv1Power > 20000 || pv2Power < 0 || pv2Power > 20000 ||
        totalPvPower < 0 || totalPvPower > 20000) {
      return INVALID_PV_POWER;
    }
    if (values[10] > 10000 || values[11] > 10000) {
      return INVALID_PV_VOLTAGE;
    }
    // 100 A per MPPT is deliberately generous and catches only bad encoding.
    if (values[14] > 1000 || values[15] > 1000) {
      return INVALID_PV_CURRENT;
    }
  }
  return INVALID_NONE;
}

// IMEON Modbus protocol vA.29 defines the exact number of words accepted by
// each writable command. Reject partial, oversized and undocumented commands
// before they reach the RTU bus.
const RegisterRange writableCommands[] = {
    {0x1000, 1}, {0x1001, 4},
    {0x1100, 2},
    {0x1300, 2},
    {0x1303, 1}, {0x1304, 1}, {0x1305, 1}, {0x1306, 1},
    {0x1307, 1}, {0x1308, 1}, {0x1309, 2}, {0x130B, 1},
    {0x130C, 1}, {0x130D, 1}, {0x130E, 1}, {0x130F, 1},
    {0x1310, 1}, {0x1311, 1}, {0x1312, 1}, {0x1313, 1},
    {0x1314, 1}, {0x1315, 1},
    {0x1316, 3}, {0x1319, 3}, {0x131C, 3}, {0x131F, 3},
    {0x1321, 1}, {0x1322, 1}, {0x1323, 1}, {0x1324, 1},
    {0x1405, 4}, {0x1409, 4}, {0x140D, 5},
    {0x1417, 1}, {0x1418, 1}, {0x1419, 2},
    {0x1421, 2}, {0x1423, 5}
};
const int writableCommandCount = sizeof(writableCommands) / sizeof(writableCommands[0]);
const uint8_t imeonActivationKey[8] = {IMEON_ACTIVATION_KEY_BYTES};

bool isDocumentedWrite(uint16_t address, uint16_t registerCount) {
  for (int i = 0; i < writableCommandCount; i++) {
    if (writableCommands[i].start == address && writableCommands[i].length == registerCount) {
      return true;
    }
  }
  return false;
}

uint8_t commandByte(const uint16_t* values, uint8_t index) {
  uint16_t word = values[index / 2];
  return index % 2 == 0 ? highByte(word) : lowByte(word);
}

bool isMeterControlWrite(uint16_t address) {
  return address == 0x1316 || address == 0x1319 || address == 0x131C ||
      address == 0x131F || address == 0x140D || address == 0x1421 ||
      address == 0x1423;
}

bool isValidCompliance(uint16_t value) {
  const uint16_t validValues[] = {
      0x0001, 0x0003, 0x0005, 0x0007, 0x0009, 0x000B, 0x000C,
      0x000D, 0x000E, 0x000F, 0x0010, 0x0011, 0x0012, 0x0013,
      0x0015, 0x0016, 0x0017
  };
  for (uint16_t validValue : validValues) {
    if (value == validValue) return true;
  }
  return false;
}

bool isValidWriteValue(uint16_t address, uint16_t registerCount, const uint16_t* values) {
  if (values == nullptr || !isDocumentedWrite(address, registerCount)) return false;

  switch (address) {
    case 0x1000:
      return values[0] == 0 || values[0] == 30 || values[0] == 60 ||
          values[0] == 300 || values[0] == 600;
    case 0x1001:
      return values[0] >= 2000 && values[0] <= 2099 &&
          commandByte(values, 2) >= 1 && commandByte(values, 2) <= 12 &&
          commandByte(values, 3) >= 1 && commandByte(values, 3) <= 31 &&
          commandByte(values, 4) <= 23 && commandByte(values, 5) <= 59 &&
          commandByte(values, 6) <= 59;
    case 0x1100: {
      uint32_t watts = (static_cast<uint32_t>(values[0]) << 16) | values[1];
      return watts <= 9000;
    }
    case 0x1300:
      return values[0] > 0 && values[1] <= 100;
    case 0x1303:
    case 0x1304:
      return values[0] >= 10 && values[0] <= 1600;
    case 0x1305:
      return values[0] >= 110 && values[0] <= 2000;
    case 0x1306: {
      const uint16_t validValues[] = {
          0x8000, 0x7FFF, 0x4000, 0xBFFF, 0x2000, 0xDFFF,
          0x1000, 0xEFFF, 0x0800, 0xF7FF, 0x0400, 0xFBFF,
          0x0200, 0xFDFF, 0x0100, 0xFEFF
      };
      for (uint16_t validValue : validValues) {
        if (values[0] == validValue) return true;
      }
      return false;
    }
    case 0x1307:
    case 0x1308:
      return values[0] >= 480 && values[0] <= 620;
    case 0x1309:
      return commandByte(values, 0) <= 23 && commandByte(values, 1) <= 59 &&
          commandByte(values, 2) <= 23 && commandByte(values, 3) <= 59;
    case 0x130B:
      return values[0] >= 400 && values[0] <= 510;
    case 0x130C:
      return values[0] >= 400 && values[0] <= 560;
    case 0x130D:
      return values[0] >= 420 && values[0] <= 510;
    case 0x130E:
      return values[0] >= 420 && values[0] <= 560;
    case 0x130F:
    case 0x1310:
      return values[0] >= 5 && values[0] <= 100;
    case 0x1311:
      return (values[0] >= 90 && values[0] <= 100) ||
          (values[0] >= 190 && values[0] <= 199);
    case 0x1312:
    case 0x1313:
      return values[0] >= 450 && values[0] <= 550;
    case 0x1314:
    case 0x1315:
      return values[0] >= 1800 && values[0] <= 2760;
    case 0x1316:
    case 0x1319:
    case 0x131C:
    case 0x131F:
      return true; // Six-byte signed adjustment encoding has no stated range.
    case 0x1321:
      return values[0] <= 1;
    case 0x1322:
      return isValidCompliance(values[0]);
    case 0x1323:
      return values[0] >= 1 && values[0] <= 4;
    case 0x1324:
      return values[0] == 0;
    case 0x1405:
      return values[0] >= 400 && values[0] <= 510 &&
          values[1] >= 400 && values[1] <= 560 &&
          values[2] >= 420 && values[2] <= 510 &&
          values[3] >= 420 && values[3] <= 560 &&
          values[0] < values[1] && values[2] < values[3] &&
          values[0] <= values[2] && values[1] <= values[3];
    case 0x1409:
      return values[0] >= 5 && values[0] <= 100 &&
          values[1] >= 5 && values[1] <= 100 &&
          values[2] >= 5 && values[2] <= 100 &&
          values[3] >= 5 && values[3] <= 100 &&
          values[0] < values[1] && values[2] < values[3];
    case 0x140D:
      return commandByte(values, 0) == 0 && commandByte(values, 1) <= 1 &&
          commandByte(values, 4) <= 1 && commandByte(values, 7) <= 1;
    case 0x1417:
      return values[0] == 1;
    case 0x1418:
      return values[0] >= 2350 && values[0] <= 2750;
    case 0x1419:
      return values[0] >= 350 && values[0] <= 500 &&
          values[1] >= 700 && values[1] <= 750 && values[0] < values[1];
    case 0x1421:
      return highByte(values[0]) == 0 && lowByte(values[0]) <= 1;
    case 0x1423:
      return commandByte(values, 0) == 0 && commandByte(values, 1) <= 1 &&
          commandByte(values, 4) <= 1 && commandByte(values, 7) <= 1;
    default:
      return false;
  }
}

bool enqueueWriteCommand(
    uint16_t address,
    uint16_t registerCount,
    const uint16_t* values,
    uint8_t retryCount = 0);

Modbus::ResultCode sendTcpException(
    Modbus::frame_arg_t* source,
    uint8_t functionCode,
    Modbus::ResultCode exceptionCode) {
  uint8_t response[] = {
      static_cast<uint8_t>(functionCode | 0x80),
      static_cast<uint8_t>(exceptionCode)
  };
  queueTcpResponse(source, response, sizeof(response));
  return Modbus::EX_SUCCESS;
}

void resetRebootCounter() {
  rebootCounter.store(0);
  EEPROM.writeUInt(EEPROM_REBOOT_COUNTER_ADDRESS, 0);
  EEPROM.commit();
  mbTcp.Hreg(REBOOT_COUNTER, 0);
}

Modbus::ResultCode onModbusRequest(uint8_t* data, uint8_t length, void* custom) {
  if (data == nullptr || custom == nullptr || length < 1) {
    return Modbus::EX_ILLEGAL_VALUE;
  }

  requestLedBlink(LED_TRANS);
  uint8_t functionCode = data[0];
  auto src = (Modbus::frame_arg_t*) custom;
      
  tcpTime = millis();

  switch (functionCode) {
    case 0x03: {
      if (length != 5) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
      }
      uint16_t address = (data[1] << 8) | data[2];
      LOG_DEBUG("TCPread: 0x%02X, Address: 0x%02X %d, passthrough\n", functionCode, address, address);
      return Modbus::EX_PASSTHROUGH;
    }

    case 0x06: {
      if (length != 5) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
      }
      uint16_t address = (data[1] << 8) | data[2];
      uint16_t singleRegValue = (data[3] << 8) | data[4];
      if (address == REBOOT_COUNTER) {
        if (singleRegValue != 0) {
          return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
        }
        resetRebootCounter();
        break;
      }
      if (!isDocumentedWrite(address, 1)) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_ADDRESS);
      }
      if (!isValidWriteValue(address, 1, &singleRegValue)) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
      }
      if (!enqueueWriteCommand(address, 1, &singleRegValue)) {
        return sendTcpException(src, functionCode, Modbus::EX_SLAVE_DEVICE_BUSY);
      }
      LOG_DEBUG("TCPwrite: 0x%02X, Addr=%d 0x%02X, Value=%d\n", 
                    functionCode, address, address, singleRegValue); 
      break;
    }

    case 0x10: {
      if (length < 6) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
      }
      uint16_t address = (data[1] << 8) | data[2];
      uint16_t regs = (data[3] << 8) | data[4];
      uint8_t byteCount = data[5];

      if (regs == 0 || regs > MAX_WRITE_VALUES || byteCount != regs * 2 || length != 6 + byteCount) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
      }
      bool isRebootReset = address == REBOOT_COUNTER && regs == 1;
      if (!isRebootReset && !isDocumentedWrite(address, regs)) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_ADDRESS);
      }

      uint16_t registerValues[MAX_WRITE_VALUES];
      for (uint16_t i = 0; i < regs; i++) {
        registerValues[i] = (data[6 + i*2] << 8) | data[6 + i*2 + 1];
        Serial.printf(" 0x%04X ", registerValues[i]);
      }
      Serial.println("");
      if (isRebootReset) {
        if (registerValues[0] != 0) {
          return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
        }
        resetRebootCounter();
        break;
      }
      if (!isValidWriteValue(address, regs, registerValues)) {
        return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_VALUE);
      }
      if (!enqueueWriteCommand(address, regs, registerValues)) {
        return sendTcpException(src, functionCode, Modbus::EX_SLAVE_DEVICE_BUSY);
      }
      break;
    }

    default: {
      static unsigned long lastIllegalFunctionLog = 0;
      unsigned long now = millis();
      if (lastIllegalFunctionLog == 0 ||
          now - lastIllegalFunctionLog >= ILLEGAL_FUNCTION_LOG_INTERVAL) {
        lastIllegalFunctionLog = now;
        Serial.printf("TCP illegal function received: %02X (further logs rate-limited)\n",
                      functionCode);
        LOG_ERROR("TCP illegal function received: %02X (further logs rate-limited)\n",
                  functionCode);
      }
      return sendTcpException(src, functionCode, Modbus::EX_ILLEGAL_FUNCTION);
    }
  }  

  // Construct affirmative response PDU
  uint8_t response[5]; // Function Code + Starting Address (2) + Quantity of Registers (2)
  response[0] = data[0];                   // Function Code (0x10)
  response[1] = data[1];                   // Starting Address High Byte
  response[2] = data[2];                   // Starting Address Low Byte
  response[3] = data[3];                   // Value (0x06) or quantity (0x10), high byte
  response[4] = data[4];                   // Value (0x06) or quantity (0x10), low byte
  if (!queueTcpResponse(src, response, sizeof(response))) {
    LOG_ERROR("Failed to queue TCP response for transaction: %d", src->transactionId);
  }
  return Modbus::EX_SUCCESS;
}

void requestLedBlink(int led) {
  if (ledQueue != NULL) {
    xQueueSend(ledQueue, &led, 0);
  }
}

void serviceLeds() {
  static unsigned long errorLedOffAt = 0;
  static unsigned long transactionLedOffAt = 0;
  int led = 0;
  unsigned long now = millis();

  while (xQueueReceive(ledQueue, &led, 0) == pdTRUE) {
    digitalWrite(led, HIGH);
    if (led == LED_ERR) {
      errorLedOffAt = now + LED_BLINK_DURATION;
    } else if (led == LED_TRANS) {
      transactionLedOffAt = now + LED_BLINK_DURATION;
    }
  }

  if (errorLedOffAt != 0 && static_cast<int32_t>(now - errorLedOffAt) >= 0) {
    digitalWrite(LED_ERR, LOW);
    errorLedOffAt = 0;
  }
  if (transactionLedOffAt != 0 && static_cast<int32_t>(now - transactionLedOffAt) >= 0) {
    digitalWrite(LED_TRANS, LOW);
    transactionLedOffAt = 0;
  }
}

// Send battery data to a Modbus TCP server.
// I use it to send to esp32 controller which reads smartmeter readinngs
// and answers requests from heatpump
void forwardBatteryData(const uint16_t* values) {
    unsigned long startTime = millis();
    uint16_t result = 0;

    for (uint16_t i = 0; i < BAT_NR_ADDRESSES; i++) {
        uint16_t address = BAT_START_ADDRESS + i;
        mbBat.Hreg(address, values[i]);
    }

    while (true) {
        mbBat.task();
        if (mbBat.isConnected(mbBatDestination)) {
            result = mbBat.pushHreg(mbBatDestination, BAT_START_ADDRESS, BAT_START_ADDRESS, BAT_NR_ADDRESSES);

            if (result) {
                LOG_INFO("Battery data transmission queued successfully");
                return;
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

bool queueRegisterUpdate(uint16_t address, uint16_t length, const uint16_t* values) {
  if (length == 0 || length > MAX_REGISTER_UPDATE_VALUES || values == nullptr) {
    LOG_ERROR("Invalid register update: address=0x%04X, count=%d", address, length);
    return false;
  }

  RegisterUpdate update;
  update.address = address;
  update.length = length;
  update.capturedAt = millis();
  memcpy(update.values, values, length * sizeof(uint16_t));
  if (xQueueSend(registerUpdateQueue, &update, 0) != pdTRUE) {
    LOG_ERROR("Register update queue full at address 0x%04X", address);
    return false;
  }
  return true;
}

void queueTrackingRegisters() {
  uint16_t values[TRACKING_REGISTER_COUNT] = {0};
  values[READ_COUNT_H - READ_COUNT_H] = highWord(readCount);
  values[READ_COUNT_L - READ_COUNT_H] = lowWord(readCount);
  values[READ_ERROR - READ_COUNT_H] = readError;
  values[READ_TIME - READ_COUNT_H] = readTime;
  values[MAX_READ_TIME - READ_COUNT_H] = maxReadTime;
  values[WRITE_COUNT_H - READ_COUNT_H] = highWord(writeCount);
  values[WRITE_COUNT_L - READ_COUNT_H] = lowWord(writeCount);
  values[WRITE_ERROR - READ_COUNT_H] = writeError;
  values[WRITE_TIME - READ_COUNT_H] = writeTime;
  values[MAX_WRITE_TIME - READ_COUNT_H] = maxWriteTime;
  values[ROUND_ROBIN_TIME - READ_COUNT_H] = roundRobinTime / 1000;
  values[MAX_ROUND_ROBIN_TIME - READ_COUNT_H] = maxRoundRobinTime / 1000;
  values[DROPPED_WRITE_COUNT - READ_COUNT_H] = droppedWriteCount;
  values[LAST_FAILED_WRITE_ADDRESS - READ_COUNT_H] = lastFailedWriteAddress;
  values[LAST_WRITE_RESULT - READ_COUNT_H] = lastWriteResult;
  values[INVALID_READ_COUNT - READ_COUNT_H] = invalidReadCount;
  values[LAST_INVALID_READ_ADDRESS - READ_COUNT_H] = lastInvalidReadAddress;
  values[LAST_INVALID_READ_REASON - READ_COUNT_H] = lastInvalidReadReason;
  values[WRITE_QUEUE_SIZE - READ_COUNT_H] =
      uxQueueMessagesWaiting(commandQueue) + (writeRetryPending ? 1 : 0);
  values[REBOOT_COUNTER - READ_COUNT_H] = rebootCounter.load();
  queueRegisterUpdate(READ_COUNT_H, TRACKING_REGISTER_COUNT, values);
}

void applyRegisterUpdates() {
  const uint32_t now = millis();
  ageTelemetry(now);
  RegisterUpdate update;
  while (xQueueReceive(registerUpdateQueue, &update, 0) == pdTRUE) {
    // Apply the entire snapshot before mbTcp.task() can expose it to a client.
    for (uint16_t i = 0; i < update.length; i++) {
      uint16_t address = update.address + i;
      uint16_t value = address == REBOOT_COUNTER ? rebootCounter.load() : update.values[i];
      mbTcp.Hreg(address, value);
    }
    for (int i = 0; i < rangeCount; i++) {
      if (update.address == predefinedRanges[i].start &&
          update.length == predefinedRanges[i].length) {
        rangeFreshness[i].seen = true;
        rangeFreshness[i].ageMs = now - update.capturedAt;
        break;
      }
    }
  }
  publishFreshness();
}

//----------------------Queue commands-------------------------------------------
bool enqueueWriteCommand(
    uint16_t address,
    uint16_t registerCount,
    const uint16_t* values,
    uint8_t retryCount) {
  if (registerCount == 0 || registerCount > MAX_WRITE_VALUES || values == nullptr) {
    LOG_ERROR("Invalid write command: address=0x%04X, count=%d", address, registerCount);
    return false;
  }

  WriteCommand command;
  command.address = address;
  command.length = registerCount; // store number of registers
  command.retryCount = retryCount;
  memcpy(command.values, values, registerCount * sizeof(uint16_t));
  if (xQueueSend(commandQueue, &command, 0) == pdTRUE) {
    LOG_DEBUG("ENQ Address: 0x%04X, RegNum: %d, Retry: %d, Val(s): ",
              address, registerCount, retryCount);
    for (uint16_t i = 0; i < registerCount; i++) {
      LOG_DEBUG(" 0x%04X", values[i]);
    }
    return true;
  } else {
    Serial.println("Failed to enqueue command.");
    LOG_ERROR("Failed to enqueue command.");
    return false;
  }
}


// Function to connect or reconnect to Wi-Fi
void manageWiFi() {
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

uint8_t activateImeon() {
  for (uint8_t i = 0; i < 4; i++) {
    uint16_t value =
        (static_cast<uint16_t>(imeonActivationKey[i * 2]) << 8) |
        imeonActivationKey[i * 2 + 1];
    mbImeon.setTransmitBuffer(i, value);
  }

  LOG_INFO("Sending IMEON Modbus activation frame");
  Serial.println("Sending IMEON Modbus activation frame");
  uint8_t result = mbImeon.writeMultipleRegisters(0x9000, 4);
  lastWriteCompletedAt = millis();
  if (result == 0) {
    LOG_INFO("IMEON Modbus activation acknowledged");
    Serial.println("IMEON Modbus activation acknowledged");
  } else {
    LOG_ERROR("IMEON Modbus activation failed with RTU result 0x%02X", result);
    Serial.printf("IMEON Modbus activation failed with RTU result 0x%02X\n", result);
  }
  return result;
}

void modbusRTU(void* parameter) {
  WriteCommand command;
  uint8_t result = 0;
  bool readBeforeNextWrite = false;
  bool imeonActivated = false;
  bool activationAttempted = false;
  unsigned long lastActivationAttempt = 0;

  while (true) {
    mbBat.task();
    vTaskDelay(pdMS_TO_TICKS(200));

    unsigned long now = millis();
    if (!imeonActivated) {
      if (!activationAttempted || now - lastActivationAttempt >= ACTIVATION_RETRY_INTERVAL) {
        activationAttempted = true;
        lastActivationAttempt = now;
        transactionStartTime = now;
        result = activateImeon();
        imeonActivated = result == 0;
        lastQueryTime = millis();
        queueTrackingRegisters();
      }
      continue;
    }

    // check if time has passed for next modbusRTU transaction
    if (!isRtuTransaction && (millis() - lastQueryTime > READ_QUERY_INTERVAL)) {
      isRtuTransaction = true;
      transactionStartTime = millis();

      // After a failed write, force one telemetry read before another write
      // attempt so a poisoned command cannot monopolize the RTU bus.
      bool processWrite = false;
      if (!readBeforeNextWrite &&
          (writeRetryPending || xQueuePeek(commandQueue, &command, 0) == pdTRUE)) {
        unsigned long requiredInterval = isMeterControlWrite(command.address)
            ? METER_CONTROL_MIN_INTERVAL
            : MIN_WRITE_INTERVAL;
        if (millis() - lastWriteCompletedAt > requiredInterval) {
          if (writeRetryPending) {
            processWrite = true;
            writeRetryPending = false;
          } else {
            processWrite = xQueueReceive(commandQueue, &command, 0) == pdTRUE;
          }
        }
      }
      if (processWrite) {
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
        lastWriteCompletedAt = millis();
        
        // Check the result
        if (result == 0) {
          writeCount++;
          lastWriteResult = 0;
          writeTime = millis() - transactionStartTime;
          if (maxWriteTime < writeTime) {
            maxWriteTime = writeTime;
          }
          Serial.printf("  ACKNOWLEDGED: RTU write of %d register(s) starting at 0x%04X %d, time: %d\n",
            command.length, command.address, command.address, writeTime);
          LOG_INFO("  ACKNOWLEDGED: RTU write of %d register(s) starting at 0x%04X %d, time: %d\n",
            command.length, command.address, command.address, writeTime);
        } else {
          Serial.printf("  ERROR: Write failed with code 0x%02X\n", result);
          LOG_ERROR("  ERROR: Write failed with code 0x%02X\n", result);
          writeError++;
          lastFailedWriteAddress = command.address;
          lastWriteResult = result;
          readBeforeNextWrite = true;

          if (command.retryCount < MAX_WRITE_RETRIES) {
            command.retryCount++;
            writeRetryPending = true;
          } else {
            droppedWriteCount++;
            LOG_ERROR("Dropping write at 0x%04X after %d retries",
                      command.address, command.retryCount);
          }
        }

      // if there is nothing in write_queue, proceed with next read  
      } else {
        readBeforeNextWrite = false;
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
          readTime = millis() - transactionStartTime;
          if (maxReadTime < readTime) {
            maxReadTime = readTime;
          }
          LOG_DEBUG("ReadRTU success: 0x%04X %d, length: %d, time: %d :: ", reg, reg, length, readTime);
          Serial.printf("ReadRTU success: 0x%04X %d, length: %d, time: %d\n", reg, reg, length, readTime);
          uint16_t values[MAX_REGISTER_UPDATE_VALUES];
          for (uint16_t i = 0; i < length; i++) {
            values[i] = mbImeon.getResponseBuffer(i);
          }
          const uint16_t invalidReason = validateTelemetryRange(reg, length, values);
          if (invalidReason == INVALID_NONE) {
            if (queueRegisterUpdate(reg, length, values)) {
              readCount++;
            }
            if (reg == BAT_START_ADDRESS && length >= BAT_NR_ADDRESSES) {
              forwardBatteryData(values);
            }
          } else {
            invalidReadCount++;
            lastInvalidReadAddress = reg;
            lastInvalidReadReason = invalidReason;
            LOG_ERROR(
                "Rejected implausible RTU snapshot: address=0x%04X, reason=%u",
                reg, invalidReason);
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
      queueTrackingRegisters();
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
  uint32_t storedRebootCounter = EEPROM.readUInt(EEPROM_REBOOT_COUNTER_ADDRESS);
  uint16_t currentRebootCounter =
      storedRebootCounter == 0xFFFFFFFFUL ? 0 : static_cast<uint16_t>(storedRebootCounter);
  Serial.printf("Reboot counter stored in eeprom: %d\n", currentRebootCounter);
  currentRebootCounter++; // Increment the counter
  rebootCounter.store(currentRebootCounter);
  EEPROM.writeUInt(EEPROM_REBOOT_COUNTER_ADDRESS, currentRebootCounter); // Write the updated counter back to EEPROM
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
  mbBat.addHreg(BAT_START_ADDRESS, 0, BAT_NR_ADDRESSES);
  
  Serial1.begin(BAUD_RATE, SERIAL_8N1, PIN_RX, PIN_TX); // RX = 16, TX = 17
  mbImeon.begin(MODBUS_RTU_ID, Serial1);
  mbImeon.idle(modbusIdle);

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
  mbTcp.addHreg(FRESHNESS_REGISTER_START, 0, rangeCount * 4);
  publishFreshness();
  // Initialize inter-core queues before starting the RTU task.
  commandQueue = xQueueCreate(WRITE_QUEUE_LENGTH, sizeof(WriteCommand));
  registerUpdateQueue = xQueueCreate(REGISTER_UPDATE_QUEUE_LENGTH, sizeof(RegisterUpdate));
  ledQueue = xQueueCreate(LED_QUEUE_LENGTH, sizeof(int));
  logQueue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(LogEntry));
  tcpResponseQueue = xQueueCreate(TCP_RESPONSE_QUEUE_LENGTH, sizeof(TcpResponse));
  if (commandQueue == NULL || registerUpdateQueue == NULL || ledQueue == NULL ||
      logQueue == NULL || tcpResponseQueue == NULL) {
      Serial.println("Error: Failed to create a FreeRTOS queue.");
      ESP.restart();
  } else {
      Serial.println("FreeRTOS queues successfully created.");
  }
  queueTrackingRegisters();

  // Create the processing task
  xTaskCreatePinnedToCore(
      modbusRTU,                        // Task function
      "ModbusRTUreadWriteCommandTask",  // Name of the task
      8192,                             // Stack size in bytes on ESP-IDF
      NULL,                             // Task input parameter
      1,                                // Priority
      NULL,                             // Task handle
      1                                 // Core ID (0 for core 0)
  );

}

void loop() {
  applyRegisterUpdates();
  serviceLogs();
  serviceLeds();
  mbTcp.task();
  serviceTcpResponses();
  serviceLogs();
  serviceLeds();

  unsigned long now = millis();
  if (now - lastWifiCheck >= WIFI_CHECK_INTERVAL) {
    lastWifiCheck = now;
    manageWiFi();
  }

  vTaskDelay(pdMS_TO_TICKS(TCP_TASK_INTERVAL));
}
