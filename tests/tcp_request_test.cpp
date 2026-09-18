#include <cassert>
#include <cstdio>
#include <deque>
#include <memory>
#include "ModbusTCPTemplate.h"
struct Socket { std::vector<uint8_t> input; size_t position=0; };
struct Client {
  std::shared_ptr<Socket> socket;
  Client()=default;
  explicit Client(std::shared_ptr<Socket> s):socket(s) {}
  operator bool() const { return bool(socket); }
  bool connected() const { return bool(socket); }
  IPAddress remoteIP() const { return IPAddress(1); }
  size_t available() const { return socket->input.size()-socket->position; }
  size_t readBytes(uint8_t* out,size_t size) {
    size=std::min(size,available());
    memcpy(out,socket->input.data()+socket->position,size);socket->position+=size;return size;
  }
  int read() { return available() ? socket->input[socket->position++] : -1; }
  size_t write(const uint8_t*,size_t n) { return n; }
  void flush() {}
  void stop() { socket.reset(); }
  bool connect(IPAddress,uint16_t) { return false; }
};
struct Server {
  static std::deque<Client> pending;
  explicit Server(uint16_t) {}
  void begin() {}
  Client accept() { if(pending.empty()) return {};auto c=pending.front();pending.pop_front();return c; }
};
std::deque<Client> Server::pending;

int main() {
  ModbusTCPTemplate<Server,Client> tcp;tcp.server();
  std::vector<uint16_t> ids;
  std::vector<std::vector<uint8_t>> pdus;
  tcp._cbRaw=[&](uint8_t* pdu,uint8_t length,void* context) {
    ids.push_back(static_cast<Modbus::frame_arg_t*>(context)->transactionId);
    pdus.emplace_back(pdu,pdu+length);return Modbus::EX_SUCCESS;
  };
  auto socket=std::make_shared<Socket>();
  // A complete MBAP header alone must wait; one function byte completes it.
  socket->input={0,0,0,0,0,2,1};Server::pending.emplace_back(socket);
  tcp.task();assert(ids.empty());
  socket->input.push_back(0x7f);
  tcp.task();assert(ids.size()==1 && ids[0]==0 && pdus[0]==std::vector<uint8_t>{0x7f});
  // Two minimal requests in one TCP read must both be consumed.
  std::vector<uint8_t> next={0,1,0,0,0,2,1,0x7f,0xff,0xff,0,0,0,2,1,0x7f};
  socket->input.insert(socket->input.end(),next.begin(),next.end());tcp.task();
  assert((ids==std::vector<uint16_t>{0,1,65535}));
  assert(socket->position==socket->input.size());
  // Ordinary FC03 requests continue to reach the handler unchanged.
  next={0x80,0,0,0,0,6,1,3,3,0,0,4};
  socket->input.insert(socket->input.end(),next.begin(),next.end());tcp.task();
  assert(ids.back()==32768 && pdus.back()==std::vector<uint8_t>({3,3,0,0,4}));
  puts("PASS real TCP parser: split header/function, minimal frames, coalesced frames, ordinary reads");
}
