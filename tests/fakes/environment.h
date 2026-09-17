#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#define highByte(x) uint8_t((x) >> 8)
#define lowByte(x) uint8_t(x)
#define highWord(x) uint16_t((x) >> 16)
#define lowWord(x) uint16_t(x)
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define HIGH 1
#define LOW 0
#define OUTPUT 1
#define SERIAL_8N1 0
#define WL_CONNECTED 3
#define FAC_USER 0
#define PRI_INFO 0
#define PRI_ERROR 1
#define PRI_DEBUG 2

namespace fake {
struct Stop {};
inline uint32_t now = 0;
inline uint32_t stopAt = 0;
inline std::function<void()> tick;
struct Operation { bool write; uint16_t address; std::vector<uint16_t> values; };
inline std::vector<Operation> operations;
inline std::function<uint8_t(uint16_t, const std::vector<uint16_t>&)> onWrite;
inline std::function<uint8_t(uint16_t, std::vector<uint16_t>&)> onRead;
}
inline unsigned long millis() { return fake::now; }
inline void vTaskDelay(uint32_t ms) {
  fake::now += ms;
  if (fake::tick) fake::tick();
  if (fake::stopAt && fake::now >= fake::stopAt) throw fake::Stop{};
}
inline void delay(uint32_t ms) { vTaskDelay(ms); }
inline void pinMode(int, int) {}
inline void digitalWrite(int, int) {}
inline void xTaskCreatePinnedToCore(void (*)(void*), const char*, int, void*, int, void*, int) {}
struct SerialStub {
  void begin(int, int = 0, int = 0, int = 0) {}
  explicit operator bool() const { return true; }
  template<class... T> void printf(const char*, T...) {}
  void println(const char*) {}
  void print(const char*) {}
};
inline SerialStub Serial, Serial1;
class IPAddress {
  uint32_t value;
public:
  IPAddress(uint32_t v = 0):value(v) {}
  IPAddress(int a,int b,int c,int d):value((uint32_t(a)<<24)|(b<<16)|(c<<8)|d) {}
  std::string toString() const { return "127.0.0.1"; }
  operator uint32_t() const { return value; }
};
struct WiFiStub {
  int status() const { return WL_CONNECTED; }
  void begin(const char*,const char*) {}
  void disconnect() {}
  IPAddress localIP() const { return {}; }
};
inline WiFiStub WiFi;
struct EspStub { void restart() { throw std::runtime_error("unexpected restart"); } };
inline EspStub ESP;
struct EepromStub {
  bool begin(int) { return true; }
  uint32_t readUInt(int) { return 0; }
  void writeUInt(int,uint32_t) {}
  bool commit() { return true; }
};
inline EepromStub EEPROM;
struct SimpleSyslog {
  SimpleSyslog(const char*,const char*,const char*) {}
  template<class... T> void printf(int,int,const char*,T...) {}
};
struct FakeQueue { size_t capacity, size; std::deque<std::vector<uint8_t>> items; };
using QueueHandle_t = FakeQueue*;
inline std::vector<std::unique_ptr<FakeQueue>> fakeQueues;
inline QueueHandle_t xQueueCreate(size_t cap,size_t size) {
  fakeQueues.emplace_back(new FakeQueue{cap,size,{}});
  return fakeQueues.back().get();
}
inline int xQueueSend(QueueHandle_t q,const void* data,int) {
  if(q->items.size() == q->capacity) return 0;
  auto p=static_cast<const uint8_t*>(data);
  q->items.emplace_back(p,p+q->size); return pdTRUE;
}
inline int xQueuePeek(QueueHandle_t q,void* data,int) {
  if(q->items.empty()) return 0;
  memcpy(data,q->items.front().data(),q->size); return pdTRUE;
}
inline int xQueueReceive(QueueHandle_t q,void* data,int wait) {
  if(!xQueuePeek(q,data,wait)) return 0;
  q->items.pop_front(); return pdTRUE;
}
inline size_t uxQueueMessagesWaiting(QueueHandle_t q) { return q->items.size(); }
struct Modbus {
  enum ResultCode { EX_SUCCESS=0, EX_ILLEGAL_FUNCTION=1, EX_ILLEGAL_ADDRESS=2,
    EX_ILLEGAL_VALUE=3, EX_SLAVE_DEVICE_BUSY=6, EX_PASSTHROUGH=255 };
  using FunctionCode = uint8_t;
  struct RequestData {};
  struct frame_arg_t { uint8_t unitId; uint32_t ipaddr; uint16_t transactionId; bool server; };
};
class ModbusIP: public Modbus {
public:
  std::map<uint16_t,uint16_t> registers;
  void Hreg(uint16_t a,uint16_t v) { registers[a]=v; }
  uint16_t Hreg(uint16_t a) { return registers[a]; }
  void addHreg(uint16_t a,uint16_t v=0,uint16_t n=1) { for(int i=0;i<n;i++) Hreg(a+i,v); }
  template<class T> void onConnect(T) {}
  template<class T> void onRequest(T) {}
  template<class T> void onRequestSuccess(T) {}
  template<class T> void onRaw(T) {}
  void server() {}
  void client() {}
  void task() {}
  bool isConnected(IPAddress) { return true; }
  void connect(IPAddress) {}
  uint16_t pushHreg(IPAddress,uint16_t,uint16_t,uint16_t) { return 1; }
  uint16_t setTransactionId(uint16_t t) { return t ? t : 1; }
  bool rawResponce(IPAddress,uint8_t*,uint8_t,uint8_t) { return true; }
};
class ModbusMaster {
  std::array<uint16_t,64> tx{};
  std::vector<uint16_t> rx;
public:
  void begin(int,SerialStub&) {}
  void idle(void(*)()) {}
  void setTransmitBuffer(int i,uint16_t v) { tx[i]=v; }
  uint16_t getResponseBuffer(int i) { return rx.at(i); }
  uint8_t writeMultipleRegisters(uint16_t a,uint16_t n) {
    std::vector<uint16_t> v(tx.begin(),tx.begin()+n);
    fake::operations.push_back({true,a,v});
    return fake::onWrite ? fake::onWrite(a,v) : 0;
  }
  uint8_t readHoldingRegisters(uint16_t a,uint16_t n) {
    rx.assign(n,0);
    fake::operations.push_back({false,a,{}});
    return fake::onRead ? fake::onRead(a,rx) : 0;
  }
};
