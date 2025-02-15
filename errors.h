// modbus_error_handling.h
#ifndef ERRORS_H
#define ERRORS_H

#include <ModbusMaster.h>
#include <exception>

// Function declarations
const char* modbusRTUErrorToString(uint8_t result);

// Exception class for Modbus RTU
class ModbusRTUException : public std::exception {
public:
  ModbusRTUException(const char* message, uint8_t result = 0)
      : message_(message), result_(result) {}

  const char* what() const noexcept override { return message_; }
  uint8_t getResult() const { return result_; }

private:
  const char* message_;
  uint8_t result_;
};

#endif 
