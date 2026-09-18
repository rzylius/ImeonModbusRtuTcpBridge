#pragma once
// Only the register engine and Arduino primitives are faked. The transport
// constructor, connection management, and task/parser are the real library code.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#define MODBUS_USE_STL
#define MODBUSIP_MAX_CLIENTS 4
#define MODBUSIP_MAX_READMS 1000
#define MODBUSIP_TIMEOUT 1000
#define MODBUSIP_MAXFRAME 200
#define MODBUSIP_UNIT 1
#define MODBUSTCP_PORT 502
#define INADDR_NONE 0xffffffffUL
using String=std::string;
using TAddress=uint16_t;
class IPAddress {
  uint32_t ip;
public:
  IPAddress(uint32_t value=0):ip(value) {}
  operator uint32_t() const { return ip; }
};
inline uint32_t millis() { static uint32_t t=0; return ++t; }
inline uint16_t __swap_16(uint16_t v) { return (v>>8)|(v<<8); }
class Modbus {
public:
  enum ResultCode { EX_SUCCESS=0,EX_ILLEGAL_VALUE=3,EX_SLAVE_FAILURE=4,
    EX_TIMEOUT=224,EX_UNEXPECTED_RESPONSE=225,EX_CANCEL=226,
    EX_PASSTHROUGH=255,REPLY_OFF=256 };
  using FunctionCode=uint8_t;
  struct frame_arg_t { uint8_t unitId; uint32_t ipaddr; uint16_t transactionId; bool server; };
  std::function<ResultCode(uint8_t*,uint8_t,void*)> _cbRaw;
  uint8_t* _frame=nullptr;
  uint16_t _len=0;
  ResultCode _reply=REPLY_OFF;
  bool cbEnabled=true;
  virtual uint32_t eventSource() { return 0; }
  void exceptionResponse(FunctionCode,ResultCode) { _reply=REPLY_OFF; }
  void slavePDU(uint8_t*) { _reply=REPLY_OFF; }
  void masterPDU(uint8_t*,uint8_t*,TAddress,uint8_t*) {}
};
using cbTransaction=std::function<bool(Modbus::ResultCode,uint16_t,void*)>;
