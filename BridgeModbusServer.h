#pragma once
#include <ModbusIP_ESP8266.h>

// modbus-esp8266 4.1.0's client transaction allocator reserves ID zero.
// Server replies must instead echo all 16 bits supplied by the requester.
// Use the existing server connection, without allocating a client transaction
// or touching the library's active request frame. Call after mbTcp.task().
class BridgeModbusServer : public ModbusIP {
public:
  bool sendServerResponse(IPAddress ip, uint16_t requestId, uint8_t unitId,
                          const uint8_t* pdu, uint8_t length) {
    if (pdu == nullptr || length == 0 || length > 5) return false;
    const int8_t slot = getMaster(ip);
    if (slot < 0 || !tcpclient[slot]->connected()) return false;

    uint8_t frame[12] = {
      static_cast<uint8_t>(requestId >> 8), static_cast<uint8_t>(requestId),
      0, 0, // Protocol ID
      0, static_cast<uint8_t>(length + 1), unitId
    };
    memcpy(frame + 7, pdu, length);
    const size_t frameLength = 7 + length;
    return tcpclient[slot]->write(frame, frameLength) == frameLength;
  }
};
