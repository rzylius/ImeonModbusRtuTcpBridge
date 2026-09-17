// Exercise the real patched ModbusMaster parser, including stale-buffer recovery.
#include <ModbusMaster.h>
#include <cassert>
#include <cstdio>
#include <vector>

class FakeStream: public Stream {
public:
  std::vector<uint8_t> pending, rx;
  size_t pos=0;
  int read() override { return pos<rx.size() ? rx[pos++] : -1; }
  int available() override { return rx.size()-pos; }
  size_t write(uint8_t) override { return 1; }
  void flush() override { rx=pending; pos=0; }
  void frame(std::vector<uint8_t> bytes) {
    uint16_t crc=0xffff;
    for(auto b:bytes) crc=crc16_update(crc,b);
    bytes.push_back(lowByte(crc)); bytes.push_back(highByte(crc));
    pending=bytes;
  }
  void reply(const std::vector<uint16_t>& words) {
    std::vector<uint8_t> bytes={1,3,uint8_t(words.size()*2)};
    for(auto w:words) { bytes.push_back(highByte(w)); bytes.push_back(lowByte(w)); }
    frame(bytes);
  }
};
int main() {
  FakeStream serial; ModbusMaster mb; mb.begin(1,serial);
  serial.reply({520,30,0,80});
  assert(mb.readHoldingRegisters(0x0300,4)==0);
  serial.reply({530});
  assert(mb.readHoldingRegisters(0x0300,4)==ModbusMaster::ku8MBInvalidResponseLength);
  // A rejected frame must not replace even the first word of the old snapshot.
  assert(mb.getResponseBuffer(0)==520 && mb.getResponseBuffer(1)==30);
  serial.reply({});
  assert(mb.readHoldingRegisters(0x0300,4)==ModbusMaster::ku8MBInvalidResponseLength);
  serial.frame({1,3,7,2,18,0,10,0,0,80});
  assert(mb.readHoldingRegisters(0x0300,4)==ModbusMaster::ku8MBInvalidResponseLength);
  serial.reply({530,10,0,90,12});
  assert(mb.readHoldingRegisters(0x0300,4)==ModbusMaster::ku8MBInvalidResponseLength);
  serial.frame({1,3,255,0,0});
  assert(mb.readHoldingRegisters(0x0300,4)==ModbusMaster::ku8MBInvalidResponseLength);
  serial.reply({530,10,0,90}); serial.pending.back()^=1;
  assert(mb.readHoldingRegisters(0x0300,4)==ModbusMaster::ku8MBInvalidCRC);
  serial.reply({530,10,0,90});
  assert(mb.readHoldingRegisters(0x0300,4)==0);
  assert(mb.getResponseBuffer(0)==530 && mb.getResponseBuffer(3)==90);
  serial.frame({1,0x10,0x90,0,0,4});
  for(int i=0;i<4;i++) mb.setTransmitBuffer(i,0);
  assert(mb.writeMultipleRegisters(0x9000,4)==0);
  serial.frame({1,0x83,2});
  assert(mb.readHoldingRegisters(0x0300,4)==2);
  puts("PASS real RTU parser: complete, short, empty, odd, oversized, CRC, recovery, write, exception");
}
